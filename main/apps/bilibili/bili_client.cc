// bili_client.cc — 数据层实现(设备侧)
//
// 对应 stackchan-bili 的 sim/bili_client.cpp。接口形状照搬,实现全换:
//   libcurl        -> esp_http_client
//   std::thread    -> FreeRTOS task
//   vector/string  -> 定长结构 + PSRAM 定长槽位
//   ArduinoJson    -> cJSON
//
// 最要紧的一处改动是帧缓冲。模拟器每帧 new 一个 vector;设备上 12fps 意味着
// 每秒 12 次分配/释放几十 KB —— 那是内存碎片的可靠来源。这里改成开机分配
// 一次的定长槽位轮转,跑多久都不动堆。

#include "bili_client.h"
#include "secrets.h"
#include "tdeck_bsp.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

namespace bili {
namespace {

const char* TAG = "bili";

char g_base[96] = BILI_BASE_URL;

// ── 流的尺寸 ───────────────────────────────────────────────
// 一帧 320x240 的 MJPEG 在服务端默认画质下实测 12~20KB,48KB 是宽裕的上限。
// 槽位给 4 个:够吸收网络抖动,又不至于让画面比声音晚太多(4/12 ≈ 0.33s)。
constexpr int FRAME_SLOTS = 4;
constexpr int FRAME_CAP   = 48 * 1024;
// 音频 16kHz 单声道 16bit = 32KB/s,128KB 正好缓 4 秒。
// 这是【唯一】的抗抖动缓冲:视频落后了可以丢帧,音频断一下就是能听见的咔哒。
constexpr int PCM_CAP     = 128 * 1024;
constexpr int HDR_LEN     = 12;   // 'S''C' type flags len32 pts32
constexpr uint8_t T_JPEG = 1, T_PCM = 2, T_META = 3, T_END = 4;

struct Slot { uint8_t* buf; int len; uint32_t pts; };

struct Stream {
    Slot     slot[FRAME_SLOTS] = {};
    int      head = 0, count = 0;        // 环形队列:head 是最老的那个
    uint8_t* ui = nullptr;               // UI 正在显示的那一帧(独立一份,见 take_due)
    int      ui_len = 0;

    uint8_t* pcm = nullptr;
    int      pcm_r = 0, pcm_w = 0, pcm_n = 0;

    Meta     meta{};
    volatile bool     want_stop = false;
    volatile bool     paused    = false;
    volatile bool     running   = false;
    volatile bool     ended     = false;
    volatile int64_t  consumed  = 0;     // 已写进功放的字节数 = 播放时钟
    volatile int      in_frames = 0, dropped = 0;
    char     err[64] = {};

    SemaphoreHandle_t mu = nullptr;
    TaskHandle_t      net = nullptr, aud = nullptr;
} g_st;

struct Lock {
    Lock()  { if (g_st.mu) xSemaphoreTake(g_st.mu, portMAX_DELAY); }
    ~Lock() { if (g_st.mu) xSemaphoreGive(g_st.mu); }
};

// ── HTTP:阻塞 GET,body 落 PSRAM ────────────────────────────
char* http_get(const char* url, int cap, int* out_len)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return nullptr;

    char* buf = nullptr;
    int   n = 0;
    do {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) != 200) {
            ESP_LOGW(TAG, "HTTP %d  %s", esp_http_client_get_status_code(c), url);
            break;
        }
        buf = (char*)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
        if (!buf) break;
        while (n < cap) {
            int r = esp_http_client_read(c, buf + n, cap - n);
            if (r <= 0) break;
            n += r;
        }
        buf[n] = 0;
    } while (0);

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (buf && n == 0) { free(buf); buf = nullptr; }
    if (out_len) *out_len = n;
    return buf;
}

const char* jstr(cJSON* o, const char* k, const char* dflt = "")
{
    cJSON* v = cJSON_GetObjectItem(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : dflt;
}
int64_t jnum(cJSON* o, const char* k, int64_t dflt = 0)
{
    cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : dflt;
}

// ── 列表/详情:各用一个一次性任务,结果放这儿等 UI 来取 ──
struct Pend {
    volatile bool busy = false, ready = false, failed = false;
    char err[64] = {};
};
Pend g_lp, g_dp;

Video g_list[LIST_MAX];
int   g_list_n = 0, g_list_rid = 0, g_list_pn = 1;
bool  g_list_more = false;
int   g_req_rid = 0, g_req_pn = 1, g_req_ps = LIST_MAX;

Detail g_detail{};
char   g_req_bvid[20] = {};

void list_task(void*)
{
    char url[256];
    // cw/ch 是封面尺寸,直接让服务端裁好 —— 设备上缩放又慢又难看。
    snprintf(url, sizeof(url), "%s/list?rid=%d&pn=%d&ps=%d&cw=%d&ch=%d",
             g_base, g_req_rid, g_req_pn, g_req_ps, 100, 56);   // 16:9,和列表缩略图尺寸一致
    int len = 0;
    char* body = http_get(url, 16384, &len);
    int n = 0;
    if (body) {
        cJSON* root = cJSON_Parse(body);
        free(body);
        if (root) {
            cJSON* arr = cJSON_GetObjectItem(root, "items");
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                if (n >= LIST_MAX) break;
                Video& v = g_list[n];
                snprintf(v.bvid,  sizeof(v.bvid),  "%s", jstr(it, "bvid"));
                snprintf(v.title, sizeof(v.title), "%s", jstr(it, "title"));
                snprintf(v.up,    sizeof(v.up),    "%s", jstr(it, "up"));
                snprintf(v.pic,   sizeof(v.pic),   "%s", jstr(it, "pic"));
                v.view = jnum(it, "view");
                v.dur  = (int)jnum(it, "dur");
                n++;
            }
            g_list_rid  = (int)jnum(root, "rid", g_req_rid);
            g_list_pn   = (int)jnum(root, "pn", g_req_pn);
            g_list_more = cJSON_IsTrue(cJSON_GetObjectItem(root, "more"));
            cJSON_Delete(root);
        }
    }
    g_list_n = n;
    if (n == 0) { g_lp.failed = true; snprintf(g_lp.err, sizeof(g_lp.err), "列表拉不到"); }
    g_lp.ready = true;
    g_lp.busy = false;
    vTaskDelete(nullptr);
}

void detail_task(void*)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/detail?bvid=%s&cw=152&ch=86", g_base, g_req_bvid);   // 详情页封面尺寸,服务端裁好
    char* body = http_get(url, 8192, nullptr);
    bool ok = false;
    if (body) {
        cJSON* root = cJSON_Parse(body);
        free(body);
        if (root) {
            Detail& d = g_detail;
            snprintf(d.bvid,  sizeof(d.bvid),  "%s", jstr(root, "bvid"));
            snprintf(d.title, sizeof(d.title), "%s", jstr(root, "title"));
            snprintf(d.up,    sizeof(d.up),    "%s", jstr(root, "up"));
            snprintf(d.pic,   sizeof(d.pic),   "%s", jstr(root, "pic"));
            snprintf(d.desc,  sizeof(d.desc),  "%s", jstr(root, "desc"));
            d.view = jnum(root, "view");
            d.like = jnum(root, "like");
            d.danmaku = jnum(root, "danmaku");
            d.dur = (int)jnum(root, "dur");
            ok = d.bvid[0] != 0;
            cJSON_Delete(root);
        }
    }
    if (!ok) { g_dp.failed = true; snprintf(g_dp.err, sizeof(g_dp.err), "详情拉不到"); }
    g_dp.ready = true;
    g_dp.busy = false;
    vTaskDelete(nullptr);
}

// ── 流:分帧解析 ────────────────────────────────────────────
//
// 不照搬模拟器那种"先把字节堆进 vector 再从头扫"的写法 —— 那样每个包都要
// 搬一次内存,而且缓冲会随大包膨胀。这里用状态机:先凑满 12 字节包头,
// 拿到长度后【直接把 payload 读进目的地】,中间不落地。
struct Parser {
    uint8_t  hdr[HDR_LEN];
    int      hdr_n = 0;
    uint8_t  type = 0;
    uint32_t left = 0, pts = 0;
    uint8_t* dst = nullptr;      // 当前包往哪儿写;nullptr = 丢弃这个包
    int      dst_off = 0;
    int      slot_idx = -1;      // JPEG 包占用的槽
    char     meta_buf[320];
};

// 拿一个空槽。满了就丢最老的那一帧 —— 宁可跳帧也不能让画面拖住音频。
int acquire_slot(Parser& p)
{
    Lock lk;
    if (g_st.count >= FRAME_SLOTS) {
        g_st.head = (g_st.head + 1) % FRAME_SLOTS;
        g_st.count--;
        g_st.dropped++;
    }
    int idx = (g_st.head + g_st.count) % FRAME_SLOTS;
    (void)p;
    return idx;
}

void commit_slot(int idx, int len, uint32_t pts)
{
    Lock lk;
    g_st.slot[idx].len = len;
    g_st.slot[idx].pts = pts;
    g_st.count++;
    g_st.in_frames++;
}

void pcm_push(const uint8_t* p, int n)
{
    Lock lk;
    for (int i = 0; i < n; i++) {
        if (g_st.pcm_n >= PCM_CAP) break;        // 满了就丢:听感上比阻塞网络线程好
        g_st.pcm[g_st.pcm_w] = p[i];
        g_st.pcm_w = (g_st.pcm_w + 1) % PCM_CAP;
        g_st.pcm_n++;
    }
}

void parse_meta(const char* js, int len)
{
    char tmp[321];
    int n = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    memcpy(tmp, js, n); tmp[n] = 0;
    cJSON* root = cJSON_Parse(tmp);
    if (!root) return;
    Lock lk;
    g_st.meta.w    = (int)jnum(root, "w", 320);
    g_st.meta.h    = (int)jnum(root, "h", 240);
    g_st.meta.fps  = (int)jnum(root, "fps", 12);
    g_st.meta.rate = (int)jnum(root, "rate", 16000);
    g_st.meta.ch   = (int)jnum(root, "ch", 1);
    g_st.meta.dur  = (int)jnum(root, "dur", 0);
    snprintf(g_st.meta.src, sizeof(g_st.meta.src), "%s", jstr(root, "src"));
    g_st.meta.valid = true;
    cJSON_Delete(root);
}

// 喂一段刚收到的字节。返回 false = 要求中止。
bool feed(Parser& p, const uint8_t* buf, int n)
{
    int off = 0;
    while (off < n) {
        if (g_st.want_stop) return false;

        if (p.left == 0) {                      // 还在凑包头
            while (p.hdr_n < HDR_LEN && off < n) p.hdr[p.hdr_n++] = buf[off++];
            if (p.hdr_n < HDR_LEN) break;
            if (p.hdr[0] != 'S' || p.hdr[1] != 'C') {
                // 错位了。丢一个字节重新找 —— 和模拟器一样的容错。
                memmove(p.hdr, p.hdr + 1, HDR_LEN - 1);
                p.hdr_n = HDR_LEN - 1;
                continue;
            }
            p.type = p.hdr[2];
            uint32_t len;
            memcpy(&len,   p.hdr + 4, 4);
            memcpy(&p.pts, p.hdr + 8, 4);
            p.hdr_n = 0;
            p.dst = nullptr; p.dst_off = 0; p.slot_idx = -1;

            if (p.type == T_END) { g_st.ended = true; return false; }
            if (len == 0) continue;
            p.left = len;

            if (p.type == T_JPEG) {
                if ((int)len <= FRAME_CAP) {
                    p.slot_idx = acquire_slot(p);
                    p.dst = g_st.slot[p.slot_idx].buf;
                } else {
                    ESP_LOGW(TAG, "帧太大 %u 字节,丢弃", (unsigned)len);
                }
            } else if (p.type == T_META) {
                p.dst = (uint8_t*)p.meta_buf;
                if (len > sizeof(p.meta_buf)) p.dst = nullptr;
            }
            // T_PCM 不设 dst:它直接进环形缓冲,见下面
            continue;
        }

        int take = n - off;
        if (take > (int)p.left) take = (int)p.left;

        if (p.type == T_PCM) {
            pcm_push(buf + off, take);
        } else if (p.dst) {
            memcpy(p.dst + p.dst_off, buf + off, take);
            p.dst_off += take;
        }
        off    += take;
        p.left -= take;

        if (p.left == 0) {                      // 一个包收全了
            if (p.type == T_JPEG && p.slot_idx >= 0) commit_slot(p.slot_idx, p.dst_off, p.pts);
            else if (p.type == T_META && p.dst)      parse_meta(p.meta_buf, p.dst_off);
        }
    }
    return true;
}

// ── 音频任务:把 PCM 喂给功放,并推进播放时钟 ──────────────
//
// 它就是【主时钟】。tdeck_speaker_write 写满 DMA 就阻塞,所以 consumed 的
// 增长天然是实时速率 —— 不需要另外拿定时器去算播放位置。
void audio_task(void*)
{
    int16_t chunk[512];
    bool inited = false;
    while (!g_st.want_stop) {
        if (!inited) {
            Meta m;
            { Lock lk; m = g_st.meta; }
            if (!m.valid) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            tdeck_audio_init(m.rate, m.ch);
            inited = true;
            ESP_LOGI(TAG, "音频:%dHz %d 声道", m.rate, m.ch);
        }
        if (g_st.paused) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }

        int got = 0;
        {
            Lock lk;
            // 起播前先攒一点再出声,否则网络一抖就是咔哒声
            int want = (int)sizeof(chunk);
            if (g_st.consumed == 0 && g_st.pcm_n < 16000) want = 0;
            while (got < want && g_st.pcm_n > 0) {
                ((uint8_t*)chunk)[got++] = g_st.pcm[g_st.pcm_r];
                g_st.pcm_r = (g_st.pcm_r + 1) % PCM_CAP;
                g_st.pcm_n--;
            }
        }
        if (got < 2) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        tdeck_speaker_write(chunk, got / 2, 300);
        g_st.consumed += got;
    }
    g_st.aud = nullptr;
    vTaskDelete(nullptr);
}

void net_task(void* arg)
{
    char* url = (char*)arg;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);

    // 收包缓冲放 PSRAM:网络线程栈只有 6K,4KB 的局部数组会顶掉大半。
    uint8_t* scratch = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    Parser* p = (Parser*)heap_caps_calloc(1, sizeof(Parser), MALLOC_CAP_SPIRAM);

    do {
        if (!c || !scratch || !p) { snprintf(g_st.err, sizeof(g_st.err), "内存不足"); break; }
        if (esp_http_client_open(c, 0) != ESP_OK) {
            snprintf(g_st.err, sizeof(g_st.err), "连不上服务");
            break;
        }
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        if (code != 200) { snprintf(g_st.err, sizeof(g_st.err), "HTTP %d", code); break; }

        int empty = 0;
        while (!g_st.want_stop) {
            int n = esp_http_client_read(c, (char*)scratch, 4096);
            if (n > 0) {
                empty = 0;
                if (!feed(*p, scratch, n)) break;
            } else {
                // read 返回 0 不一定是流结束,也可能只是这一刻没数据。
                // 直接 break 的话,视频会在开播两秒后"播完"(音乐那边踩过同样的坑)。
                if (esp_http_client_is_complete_data_received(c)) break;
                if (++empty > 150) { snprintf(g_st.err, sizeof(g_st.err), "服务端断流"); break; }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    } while (0);

    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    free(scratch);
    free(p);
    free(url);
    g_st.running = false;
    g_st.net = nullptr;
    ESP_LOGI(TAG, "流结束:收 %d 帧,丢 %d", g_st.in_frames, g_st.dropped);
    vTaskDelete(nullptr);
}

bool ensure_bufs()
{
    if (!g_st.mu) g_st.mu = xSemaphoreCreateMutex();
    if (!g_st.pcm) g_st.pcm = (uint8_t*)heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM);
    if (!g_st.ui)  g_st.ui  = (uint8_t*)heap_caps_malloc(FRAME_CAP, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < FRAME_SLOTS; i++)
        if (!g_st.slot[i].buf)
            g_st.slot[i].buf = (uint8_t*)heap_caps_malloc(FRAME_CAP, MALLOC_CAP_SPIRAM);
    if (!g_st.mu || !g_st.pcm || !g_st.ui) return false;
    for (int i = 0; i < FRAME_SLOTS; i++) if (!g_st.slot[i].buf) return false;
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────
void set_base(const char* url) { snprintf(g_base, sizeof(g_base), "%s", url); }
const char* base() { return g_base; }

int get_regions(Region* out, int max)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/regions", g_base);
    int n = 0;
    char* body = http_get(url, 4096, nullptr);
    if (body) {
        cJSON* root = cJSON_Parse(body);
        free(body);
        if (root) {
            cJSON* arr = cJSON_GetObjectItem(root, "regions");
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                if (n >= max) break;
                out[n].rid = (int)jnum(it, "rid");
                snprintf(out[n].name, sizeof(out[n].name), "%s", jstr(it, "name"));
                n++;
            }
            cJSON_Delete(root);
        }
    }
    if (n == 0) {
        // 服务没起来也要进得了界面 —— 空白列表比兜底表更像"坏了"。
        static const struct { int rid; const char* nm; } FB[] = {
            { 0, "推荐" }, { 1, "动画" }, { 3, "音乐" }, { 4, "游戏" },
            { 36, "科技" }, { 160, "生活" },
        };
        for (; n < (int)(sizeof(FB) / sizeof(FB[0])) && n < max; n++) {
            out[n].rid = FB[n].rid;
            snprintf(out[n].name, sizeof(out[n].name), "%s", FB[n].nm);
        }
    }
    return n;
}

void request_list(int rid, int pn, int ps)
{
    if (g_lp.busy) return;
    g_req_rid = rid; g_req_pn = pn; g_req_ps = ps > LIST_MAX ? LIST_MAX : ps;
    g_lp.busy = true; g_lp.ready = false; g_lp.failed = false;
    if (xTaskCreate(list_task, "bili_list", 6144, nullptr, 4, nullptr) != pdPASS) {
        g_lp.busy = false; g_lp.failed = true;
        snprintf(g_lp.err, sizeof(g_lp.err), "任务起不来");
    }
}

bool list_pending() { return g_lp.busy; }

bool poll_list(Video* out, int* n, int* rid, int* pn, bool* more)
{
    if (!g_lp.ready) return false;
    g_lp.ready = false;
    if (g_lp.failed) return false;
    memcpy(out, g_list, sizeof(Video) * g_list_n);
    *n = g_list_n; *rid = g_list_rid; *pn = g_list_pn; *more = g_list_more;
    return true;
}

bool list_failed(const char** err)
{
    if (!g_lp.failed) return false;
    g_lp.failed = false;
    if (err) *err = g_lp.err;
    return true;
}

void request_detail(const char* bvid)
{
    if (g_dp.busy) return;
    snprintf(g_req_bvid, sizeof(g_req_bvid), "%s", bvid ? bvid : "");
    g_dp.busy = true; g_dp.ready = false; g_dp.failed = false;
    if (xTaskCreate(detail_task, "bili_detail", 6144, nullptr, 4, nullptr) != pdPASS) {
        g_dp.busy = false; g_dp.failed = true;
    }
}

bool poll_detail(Detail* out)
{
    if (!g_dp.ready) return false;
    g_dp.ready = false;
    if (g_dp.failed) return false;
    *out = g_detail;
    return true;
}

bool detail_failed(const char** err)
{
    if (!g_dp.failed) return false;
    g_dp.failed = false;
    if (err) *err = g_dp.err;
    return true;
}

uint8_t* fetch_cover(const char* url, int* len)
{
    if (!url || !url[0]) return nullptr;
    // 走服务端的 /cover 代理:设备只跟一个 IP 说话,不碰 DNS。
    // (真服务的注释里记着这条 —— 设备直连 i2.hdslb.com 会卡死在 DNS 上,
    //  既不成功也不失败,一条日志都没有。)
    // ⚠️ 内层 url 必须转义。B站 封面地址带 @ 和 _(如 ...jpg@100w_56h_1c.jpg),
    // 真出现 & 的时候不转义会把查询串截断,服务端只收到半截地址。
    static const char* HEX = "0123456789ABCDEF";
    char enc[300];
    size_t o = 0;
    for (const unsigned char* s = (const unsigned char*)url; *s && o + 4 < sizeof(enc); s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || strchr("-_.~", *s)) enc[o++] = (char)*s;
        else { enc[o++] = '%'; enc[o++] = HEX[*s >> 4]; enc[o++] = HEX[*s & 15]; }
    }
    enc[o] = 0;
    char full[420];
    snprintf(full, sizeof(full), "%s/cover?url=%s", g_base, enc);
    return (uint8_t*)http_get(full, 64 * 1024, len);
}

void stream_start(const char* bvid, int rate)
{
    stream_stop();
    if (!ensure_bufs()) { snprintf(g_st.err, sizeof(g_st.err), "PSRAM 不够"); return; }

    { Lock lk;
      g_st.head = g_st.count = 0;
      g_st.pcm_r = g_st.pcm_w = g_st.pcm_n = 0;
      g_st.meta = Meta{};
      g_st.ui_len = 0;
    }
    g_st.err[0] = 0;
    g_st.consumed = 0;
    g_st.in_frames = g_st.dropped = 0;
    g_st.want_stop = false;
    g_st.paused = false;
    g_st.ended = false;
    g_st.running = true;

    char* url = (char*)malloc(256);
    if (!url) { g_st.running = false; return; }
    // 只报【设备型号】,尺寸/帧率/采样率/画质全由服务端的 DEVICES 档位决定。
    // 这些值是按设备实测调出来的,散在固件里的话换一台设备就要重烧一次;
    // 放服务端改一行重启就生效。rate 仍然留着当显式覆盖的后路。
    (void)rate;
    snprintf(url, 256, "%s/stream?bvid=%s&device=tdeck", g_base, bvid);

    if (xTaskCreate(net_task, "bili_net", 6144, url, 5, &g_st.net) != pdPASS) {
        free(url); g_st.running = false;
        snprintf(g_st.err, sizeof(g_st.err), "网络任务起不来");
        return;
    }
    // 音频任务优先级比网络高一档:音频一旦欠数据就是能听见的咔哒,
    // 而网络晚几毫秒只是多缓冲一点。
    if (xTaskCreate(audio_task, "bili_aud", 4096, nullptr, 6, &g_st.aud) != pdPASS)
        snprintf(g_st.err, sizeof(g_st.err), "音频任务起不来");
}

void stream_stop()
{
    if (!g_st.net && !g_st.aud) return;
    g_st.want_stop = true;
    for (int i = 0; i < 250 && (g_st.net || g_st.aud); i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (g_st.net || g_st.aud) ESP_LOGE(TAG, "流任务停不下来");
    // vTaskDelete 只是标记,栈由 idle 回收。让一步再继续,免得紧接着
    // 重新起任务时两份栈并存(音乐那边量过这件事)。
    vTaskDelay(pdMS_TO_TICKS(60));
    g_st.want_stop = false;
    g_st.running = false;
}

bool stream_running() { return g_st.running; }

Meta stream_meta() { Lock lk; return g_st.meta; }

bool stream_take_due(uint32_t pos_ms, const uint8_t** jpeg, int* len, uint32_t* pts)
{
    Lock lk;
    bool got = false;
    uint32_t got_pts = 0;
    while (g_st.count > 0) {
        Slot& s = g_st.slot[g_st.head];
        // 还没到时候就别动它 —— 但如果一帧都还没显示过,先显示一帧,
        // 否则起播那几百毫秒是黑屏。
        if (got && s.pts > pos_ms + 80) break;
        memcpy(g_st.ui, s.buf, s.len);
        g_st.ui_len = s.len;
        got_pts = s.pts;
        got = true;
        g_st.head = (g_st.head + 1) % FRAME_SLOTS;
        g_st.count--;
        if (s.pts > pos_ms + 80) break;   // 这帧就是"下一帧",显示它然后停
    }
    if (!got) return false;
    *jpeg = g_st.ui;
    *len  = g_st.ui_len;
    if (pts) *pts = got_pts;
    return true;
}

void stream_set_paused(bool paused) { g_st.paused = paused; }
bool stream_paused() { return g_st.paused; }

uint32_t stream_audio_pos_ms()
{
    int rate = 16000, ch = 1;
    { Lock lk; if (g_st.meta.valid) { rate = g_st.meta.rate; ch = g_st.meta.ch; } }
    return (uint32_t)(g_st.consumed * 1000LL / ((int64_t)rate * ch * 2));
}

void stream_stats(int* frames_in, int* dropped, int* pcm_buffered)
{
    if (frames_in) *frames_in = g_st.in_frames;
    if (dropped)   *dropped   = g_st.dropped;
    if (pcm_buffered) { Lock lk; *pcm_buffered = g_st.pcm_n; }
}

bool stream_error(const char** err)
{
    if (!g_st.err[0]) return false;
    if (err) *err = g_st.err;
    return true;
}

}  // namespace bili
