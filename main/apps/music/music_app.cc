// music_app.cc — 音乐播放器
//
// 后端是 nuc 上自建的 xiaozhi-music-mcp。它把所有音频统一转成
// 24kHz 单声道 Ogg/Opus(约 4~8 KB/s),正是为 ESP32 这点算力和带宽设计的 ——
// 固件不用重采样、不用切采样率,拉下来解码直接送 I2S。
//
// 列表走 /api/reco?kind=mine(最近播放),音频走 /audio?f=<key>.opus。
// 两个接口都用 ?key=<token> 鉴权,token 在 secrets.h 里(不进版本库)。

#include "app.h"
#include <math.h>
#include "secrets.h"
#include "tdeck_bsp.h"
#include "ui/fonts.h"

#include <cJSON.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <decoder/impl/esp_opus_dec.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <inttypes.h>
#include "sys/volume.h"
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <string.h>

namespace {

// 卡片配图。tools/gen_card_art.py 生成,CMake EMBED_FILES 以二进制嵌入
// (143x94 RGB565,26884 字节),不走 C 数组。
extern "C" const uint8_t card_music_start[] asm("_binary_card_music_rgb565_start");
const lv_image_dsc_t kCardArt = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = 143, .h = 94, .stride = 143 * 2, .reserved_2 = 0 },
    .data_size = 143 * 94 * 2,
    .data      = card_music_start,
};

const char* TAG = "music";
// 中文字体没加载成功就回落到 Montserrat —— 宁可显示方框,不要空指针崩溃
// ⚠️ 中文字库只有【一个】字号:20px,line_height = 26。
// 所以 F16() 和 F20() 拿到的是同一个字体,名字里的 16/20 只是回落到
// Montserrat 时的字号。排版时高度一律按 CJK_LINE_H 算,按 16px 估会
// 让文字从标签框里溢出去,盖住下一行。
constexpr int CJK_LINE_H = 26;
inline const lv_font_t* F16() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_16; }
inline const lv_font_t* F20() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_20; }
// 图标(LV_SYMBOL_*)是 FontAwesome 的私用区码位,中文字库里【没有】,
// 用 F16() 画会变成缺字形方框。图标一律用 Montserrat。
inline const lv_font_t* FICON() { return &lv_font_montserrat_16; }
// 16px 中文。列表里的歌名/歌手这类次要信息用它 —— 20px 当正文太大,
// 320 宽的屏上一行放不下几个字。拿不到就回落 20px(fonts.cc 里自检)。
inline const lv_font_t* FS()    { auto* f = tdeck::font_cjk_small(); return f ? f : F16(); }
inline int SMALL_LH() { return FS()->line_height; }



constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_TEXT   = 0x1b2117;
constexpr uint32_t C_MUTE   = 0x6a7360;
constexpr uint32_t C_LINE   = 0xd6dbcd;
constexpr uint32_t C_CARD   = 0xffffff;
constexpr int SCR_W = 320, SCR_H = 240;
constexpr int ROW_H = 34;      // 一行文字(26)+ 上下留白

constexpr int MAX_SONGS   = 30;    // 三个分区各取 10 首
constexpr int SEC_N       = 3;
constexpr int SEC_CAP     = 8;    // 每排 8 首:再多就是内部堆里的对象数在涨
constexpr int COVER_PX    = 120;   // 播放页大图
constexpr int THUMB_PX    = 88;    // 封面墙上的方形封面
constexpr int COVER_R     = 8;     // 封面圆角。占位块、图片、选中框共用一个值,
                                   // 三者半径不一致的话选中框会和图错开一圈
constexpr int SAMPLE_RATE = 24000;

struct Song {
    char key[40];
    char title[64];
    char artist[48];
    // 服务端给的封面地址,【不能自己按 key 拼】。
    // 已缓存的歌是 /cover?f=<key>.jpg,而"热门新歌"这类还没落盘的歌
    // 走的是 /thumb?u=<远程地址>&w=... —— 对后者拼 /cover?f=key.jpg 会 404,
    // 结果就是整排封面都是占位图。
    char cover[144];
    int  dur;
};

// 播放状态放在文件作用域:卡片要读它,而卡片是 launcher 在 app 关闭时画的,
// 那时 MusicApp 实例的 UI 已经拆了。放成员变量会让"关掉 app 就不知道在放什么"。
struct {
    bool   playing;
    char   title[64];
    char   artist[48];
    int    pos_s, dur_s;
    int    n_songs;
} g_state;

Song  g_songs[MAX_SONGS];

// 分区只是【歌曲池上的一段区间】,不另存一份歌。
// 这样 play_index()/搜索/正在播放高亮这些按下标走的逻辑全都不用动。
struct Section {
    const char* title;
    const char* json_key;   // 服务端 /api/reco?kind=mine 里的字段名
    int         start;      // 在 g_songs 里的起点
    int         count;
};
// "已缓存"这个分区名有歧义 —— 缓存是实现细节,不是一种听歌的理由,
// 而且它和前两个分区大量重叠(听过的自然就被缓存了)。换成三个语义不重叠的:
//   最近播放 ← mine.recent
//   最常播放 ← mine.most   服务端【没有】真正的收藏列表,播放次数最多是最接近的
//   热门新歌 ← new.items   另一个 kind,来源是网易云的新歌榜
Section g_sec[SEC_N] = {
    { "最近播放", "recent", 0, 0 },
    { "最常播放", "most",   0, 0 },
    { "热门新歌", "items",  0, 0 },   // 来自 /api/reco?kind=new
};
volatile bool g_stop_req = false;
TaskHandle_t  g_play_task = nullptr;

// ── HTTP 取 JSON ──────────────────────────────────────────
// 返回堆上的字符串,调用方 free。失败返回 nullptr。
char* http_get(const char* url, int max_len)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return nullptr;

    char* buf = (char*)heap_caps_malloc(max_len, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(cli); return nullptr; }

    int len = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        // 必须循环读到底:单次 read 只保证返回"已到达的"数据
        while (len < max_len - 1) {
            int n = esp_http_client_read(cli, buf + len, max_len - 1 - len);
            if (n <= 0) break;
            len += n;
        }
    }
    esp_http_client_cleanup(cli);
    if (len <= 0) { free(buf); return nullptr; }
    buf[len] = 0;
    return buf;
}

void fetch_list()
{
    char url[256];
    snprintf(url, sizeof(url), MUSIC_BASE_URL "/api/reco?kind=mine&key=" MUSIC_TOKEN);
    // 实测 /api/reco 的响应接近 70KB。缓冲不够的话会在中间截断,
    // 表现是"JSON 解析失败"而不是"响应太大" —— 很容易往解析逻辑上找错。
    constexpr int CAP = 160 * 1024;
    char* body = http_get(url, CAP);
    if (!body) { ESP_LOGW(TAG, "歌单请求失败"); return; }
    int blen = (int)strlen(body);
    if (blen >= CAP - 1) ESP_LOGW(TAG, "歌单响应被截断(%d 字节),缓冲要加大", blen);

    cJSON* root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "歌单 JSON 解析失败"); return; }

    // 服务端一次就把 Apple Music 那种分区结构给全了:
    //   recent / most / cached / playlists / kids_recent / kids_most
    // 这里取前三个歌曲分区,每个最多 10 首,平铺进同一个 g_songs 池,
    // 分区只记录区间。
    int n = 0;
    for (int si = 0; si < SEC_N - 1; si++) {
        g_sec[si].start = n;
        g_sec[si].count = 0;
        cJSON* arr = cJSON_GetObjectItem(root, g_sec[si].json_key);
        if (!cJSON_IsArray(arr)) continue;
        cJSON* it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= MAX_SONGS || g_sec[si].count >= SEC_CAP) break;
            cJSON* k = cJSON_GetObjectItem(it, "key");
            cJSON* tt = cJSON_GetObjectItem(it, "title");
            cJSON* a = cJSON_GetObjectItem(it, "artist");
            cJSON* d = cJSON_GetObjectItem(it, "dur");
            if (!cJSON_IsString(k) || !cJSON_IsString(tt)) continue;
            strncpy(g_songs[n].key,   k->valuestring,  sizeof(g_songs[n].key) - 1);
            strncpy(g_songs[n].title, tt->valuestring, sizeof(g_songs[n].title) - 1);
            strncpy(g_songs[n].artist,
                    cJSON_IsString(a) ? a->valuestring : "", sizeof(g_songs[n].artist) - 1);
            g_songs[n].dur = cJSON_IsNumber(d) ? d->valueint : 0;
            cJSON* cv = cJSON_GetObjectItem(it, "cover");
            strncpy(g_songs[n].cover,
                    cJSON_IsString(cv) ? cv->valuestring : "", sizeof(g_songs[n].cover) - 1);
            n++;
            g_sec[si].count++;
        }
        ESP_LOGI(TAG, "分区 %s:%d 首", g_sec[si].title, g_sec[si].count);
    }
    g_state.n_songs = n;

    cJSON_Delete(root);
    // 第三个分区在另一个 kind 里。单独发一次请求,失败就少一排,不影响前两排。
    {
        const int SI = SEC_N - 1;
        g_sec[SI].start = n;
        g_sec[SI].count = 0;
        snprintf(url, sizeof(url), MUSIC_BASE_URL "/api/reco?kind=new&key=" MUSIC_TOKEN);
        char* nb = http_get(url, 64 * 1024);
        if (nb) {
            cJSON* nr = cJSON_Parse(nb);
            free(nb);
            if (nr) {
                cJSON* arr = cJSON_GetObjectItem(nr, "items");
                cJSON* it;
                if (cJSON_IsArray(arr)) cJSON_ArrayForEach(it, arr) {
                    if (n >= MAX_SONGS || g_sec[SI].count >= SEC_CAP) break;
                    cJSON* k  = cJSON_GetObjectItem(it, "key");
                    cJSON* tt = cJSON_GetObjectItem(it, "title");
                    cJSON* a  = cJSON_GetObjectItem(it, "artist");
                    cJSON* d  = cJSON_GetObjectItem(it, "dur");
                    if (!cJSON_IsString(k) || !cJSON_IsString(tt)) continue;
                    strncpy(g_songs[n].key,   k->valuestring,  sizeof(g_songs[n].key) - 1);
                    strncpy(g_songs[n].title, tt->valuestring, sizeof(g_songs[n].title) - 1);
                    strncpy(g_songs[n].artist,
                            cJSON_IsString(a) ? a->valuestring : "", sizeof(g_songs[n].artist) - 1);
                    g_songs[n].dur = cJSON_IsNumber(d) ? d->valueint : 0;
                    cJSON* cv = cJSON_GetObjectItem(it, "cover");
                    strncpy(g_songs[n].cover,
                            cJSON_IsString(cv) ? cv->valuestring : "", sizeof(g_songs[n].cover) - 1);
                    n++; g_sec[SI].count++;
                }
                cJSON_Delete(nr);
            }
        }
        ESP_LOGI(TAG, "分区 %s:%d 首", g_sec[SI].title, g_sec[SI].count);
        g_state.n_songs = n;
    }

}

// ── 播放 ──────────────────────────────────────────────────
// 边拉边解边送,不落盘也不整曲缓存:一首 4 分钟的歌就算 8KB/s 也有 2MB,
// PSRAM 放得下但没必要 —— 流式的话内存占用是常数,而且点下去立刻出声。
char g_play_url[400];

void play_task(void* arg)
{
    (void)arg;
    esp_http_client_config_t cfg = {};
    cfg.url = g_play_url;
    cfg.timeout_ms = 12000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { g_play_task = nullptr; vTaskDelete(nullptr); }

    esp_audio_simple_dec_handle_t dec = nullptr;
    constexpr int IN_CAP = 8192, OUT_CAP = 16384;
    uint8_t* in  = (uint8_t*)heap_caps_malloc(IN_CAP, MALLOC_CAP_SPIRAM);
    uint8_t* out = (uint8_t*)heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);

    do {
        if (!in || !out) break;
        if (esp_http_client_open(cli, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(cli);

        // 【两层都要注册】。simple_dec 那一层管的是容器(Ogg 解复用),
        // 真正解 Opus 帧的是元素解码器,在另一个注册表里。
        // 只注册前者的话,Ogg 能正确识别出流是 Opus,然后报
        //     Decoder OPUS(1398100047) not registered
        // 这个错很容易被当成"容器不支持"而去换格式,实际只差一行注册。
        esp_audio_simple_dec_register_default();
        esp_opus_dec_register();

        esp_audio_simple_dec_cfg_t dcfg = {};
        dcfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        esp_audio_err_t oe = esp_audio_simple_dec_open(&dcfg, &dec);
        if (oe != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "解码器打开失败 err=%d", (int)oe);
            break;
        }
        ESP_LOGI(TAG, "开始播放:%s", g_play_url);

        // 先不配 I2S —— 等解出第一帧,拿解码器报的真实格式再配(见下)
        g_state.playing = true;
        g_state.pos_s = 0;

        int64_t samples_out = 0;
        int64_t bytes_in = 0;
        int pending = 0;
        int64_t t_log = esp_timer_get_time();
        bool first_pcm = true;
        int empty_reads = 0;
        uint32_t out_rate = SAMPLE_RATE;
        uint8_t  out_ch = 1;

        while (!g_stop_req) {
            int n = esp_http_client_read(cli, (char*)in + pending, IN_CAP - pending);
            if (n > 0) { pending += n; bytes_in += n; empty_reads = 0; }
            else if (pending == 0) {
                // read 返回 0 不一定是流结束 —— 也可能只是这一刻没数据到。
                // 直接 break 的话,一首 4 分钟的歌会在两秒钟"播完"。
                // 连续多次取不到才认定结束。
                if (esp_http_client_is_complete_data_received(cli)) break;
                if (++empty_reads > 40) break;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = in;
            raw.len    = pending;
            raw.eos    = false;

            while (raw.len > 0 && !g_stop_req) {
                // 每次调用前都要重置输出描述符。process() 会改写 o.len /
                // o.decoded_size,沿用上一轮的值会让第二次调用就出错 ——
                // 症状是只解出第一帧然后整首歌"结束"。
                esp_audio_simple_dec_out_t o = {};
                o.buffer = out;
                o.len    = OUT_CAP;

                esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &o);
                if (e != ESP_AUDIO_ERR_OK) break;

                if (o.decoded_size > 0) {
                    if (first_pcm) {
                        first_pcm = false;
                        // 问解码器要真实格式。服务端标称 24kHz 单声道,但 Opus
                        // 内部按 48kHz 工作,解出来未必是标称值 —— 按标称值配
                        // I2S 的话声音会被拉慢,而且不报任何错。
                        esp_audio_simple_dec_info_t di = {};
                        uint32_t rate = SAMPLE_RATE; uint8_t ch = 1;
                        if (esp_audio_simple_dec_get_info(dec, &di) == ESP_AUDIO_ERR_OK
                                && di.sample_rate > 0) {
                            rate = di.sample_rate;
                            ch   = di.channel ? di.channel : 1;
                        }
                        ESP_LOGI(TAG, "解码格式:%" PRIu32 "Hz %d 声道 %d bit,首帧 %u 字节",
                                 rate, ch, di.bits_per_sample, (unsigned)o.decoded_size);
                        tdeck_audio_init(rate, ch);
                        out_rate = rate; out_ch = ch;
                    }
                    tdeck_speaker_write((int16_t*)out, o.decoded_size / 2, 1000);
                    // 进度按【每声道的帧数】算,立体声时 decoded_size 里
                    // 装的是两个声道交织的样本,直接除 2 会让进度快一倍
                    samples_out += o.decoded_size / 2 / out_ch;
                    g_state.pos_s = (int)(samples_out / out_rate);
                }
                if (raw.consumed == 0) break;   // 数据不够拼一帧,回去再拉
                raw.buffer += raw.consumed;
                raw.len    -= raw.consumed;
            }

            // 没消费完的尾巴挪到开头,和下一批拼成完整帧
            if (raw.len > 0 && raw.buffer != in) memmove(in, raw.buffer, raw.len);
            pending = (int)raw.len;

            if (esp_timer_get_time() - t_log > 5000000) {
                t_log = esp_timer_get_time();
                ESP_LOGI(TAG, "已拉 %lld KB,播到 %d 秒",
                         (long long)(bytes_in / 1024), g_state.pos_s);
            }
        }
    } while (0);

    if (dec) esp_audio_simple_dec_close(dec);
    if (in)  free(in);
    if (out) free(out);
    esp_http_client_cleanup(cli);

    g_state.playing = false;
    g_play_task = nullptr;
    ESP_LOGI(TAG, "播放结束:%s", g_state.title);
    vTaskDelete(nullptr);
}

bool fetch_cover(const char* key);   // 定义在下面的封面小节

// URL 编码。键盘只能打 ASCII,所以不用管多字节 —— 但空格和 & 必须转,
// 否则查询串会被截断。
void urlencode(const char* in, char* out, size_t n)
{
    static const char* HEX = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < n; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%'; out[o++] = HEX[*p >> 4]; out[o++] = HEX[*p & 15];
        }
    }
    out[o] = 0;
}

void start_play(const char* url, const char* title, const char* artist, int dur, const char* cover_key)
{
    if (cover_key && cover_key[0]) fetch_cover(cover_key);
    strncpy(g_play_url, url, sizeof(g_play_url) - 1);
    strncpy(g_state.title,  title  ? title  : "", sizeof(g_state.title) - 1);
    strncpy(g_state.artist, artist ? artist : "", sizeof(g_state.artist) - 1);
    g_state.dur_s = dur;
    g_stop_req = false;
    // 栈给 20K:Opus 解码器在栈上开大数组,8K 会溢出;但也不能给太大 ——
    // FreeRTOS 的任务栈必须在【内部 RAM】,而这块板子内部 RAM 只剩百来 KB,
    // 32K 会直接创建失败。失败时 xTaskCreate 返回非 pdPASS,不检查的话
    // 现象是"点了播放什么都没发生、日志也没有",极难查。
    BaseType_t ok = xTaskCreate(play_task, "music_play", 20480, nullptr, 5, &g_play_task);
    if (ok != pdPASS) {
        g_play_task = nullptr;
        ESP_LOGE(TAG, "播放任务创建失败(内部 RAM 剩 %u 字节)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

void stop_playback()
{
    if (!g_play_task) return;
    g_stop_req = true;
    for (int i = 0; i < 50 && g_play_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    g_stop_req = false;
}

// 搜索走 /stream_pcm —— 服务端一次完成"四路搜索 + 标题门槛 + 打分选版 +
// 转成设备规格",返回的 audio_url 直接能播。比自己拉搜索结果再挑一遍省事得多,
// 而且和小智固件走的是同一条路径,命中的缓存也是同一份。
bool search_and_play(const char* query)
{
    char q[256], url[512];
    urlencode(query, q, sizeof(q));
    snprintf(url, sizeof(url),
             MUSIC_BASE_URL "/stream_pcm?song=%s&fmt=opus&key=" MUSIC_TOKEN, q);

    char* body = http_get(url, 4096);
    if (!body) return false;
    cJSON* root = cJSON_Parse(body);
    free(body);
    if (!root) return false;

    cJSON* au = cJSON_GetObjectItem(root, "audio_url");
    cJSON* ti = cJSON_GetObjectItem(root, "title");
    cJSON* ar = cJSON_GetObjectItem(root, "artist");
    bool ok = cJSON_IsString(au) && au->valuestring[0];
    if (ok) {
        start_play(au->valuestring,
                   cJSON_IsString(ti) ? ti->valuestring : query,
                   cJSON_IsString(ar) ? ar->valuestring : "", 0, nullptr);
    }
    cJSON_Delete(root);

    return ok;
}

// ── 封面 ──────────────────────────────────────────────────
// 服务端能按尺寸裁好再发(/cover?w=&h=),所以拉下来的就是 120x120 的小图,
// 4KB 左右。不用在设备上缩放,省掉一整套图像处理。
//
// 解码交给 LVGL 的 TJPGD:把 JPEG 原始字节包成 cf=RAW 的 image_dsc,
// LVGL 的解码器链会自己认出是 JPEG。比自己调 tjpgd 再转 RGB565 省事得多。
uint8_t*       g_cover_buf = nullptr;
lv_image_dsc_t g_cover_dsc;

// 下载一张封面 JPEG。px 是服务端裁好的边长。
// 成功返回 PSRAM 上的缓冲(调用方 free),*out_len 是长度。
// cover 是服务端给的地址:可能是绝对 URL,也可能是 "/thumb?..." 这样的相对路径。
// 空的时候退回按 key 拼(老路径,对已缓存的歌有效)。
// 把 URL 里已有的 w=/h= 参数改成我们要的边长。
// 服务端给的地址自带尺寸(/cover 是 160,/thumb 是 50),直接用的话
// 拿回来的图不是我们要的大小,还得在客户端缩放 —— 不如一开始就要对。
void rewrite_size(char* url, int px)
{
    for (const char* k : { "w=", "h=" }) {
        char* q = url;
        while ((q = strstr(q, k))) {
            // 必须是参数开头(前面是 ? 或 &),不能匹配到别的词里
            if (q != url && q[-1] != '?' && q[-1] != '&') { q += 2; continue; }
            char* v = q + 2;
            char* e = v;
            while (*e >= '0' && *e <= '9') e++;
            if (e == v) { q = v; continue; }
            char tail[420];
            snprintf(tail, sizeof(tail), "%s", e);
            int n = snprintf(v, 420 - (v - url), "%d%s", px, tail);
            q = v + (n > 0 ? n : 0);
        }
    }
}

uint8_t* fetch_jpeg_url(const char* cover, const char* key, int px, int* out_len)
{
    char url[420];
    if (cover && cover[0] == '/') {
        snprintf(url, sizeof(url), MUSIC_BASE_URL "%s&key=" MUSIC_TOKEN, cover);
        rewrite_size(url, px);
    } else if (cover && cover[0]) {
        snprintf(url, sizeof(url), "%s", cover);
        rewrite_size(url, px);
    } else {
        snprintf(url, sizeof(url),
                 MUSIC_BASE_URL "/cover?f=%s.jpg&w=%d&h=%d&key=" MUSIC_TOKEN, key, px, px);
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url; cfg.timeout_ms = 8000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return nullptr;

    const int CAP = 48 * 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(cli); return nullptr; }

    int len = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        while (len < CAP) {
            int n = esp_http_client_read(cli, (char*)buf + len, CAP - len);
            if (n <= 0) break;
            len += n;
        }
    }
    esp_http_client_cleanup(cli);
    if (len < 100) { free(buf); return nullptr; }

    // ⚠️ LVGL 的 JPEG 嗅探【只认 JFIF】。lv_tjpgd.c 的 is_jpg() 是逐字节比对
    //     FF D8 FF E0 00 10 'J' 'F' 'I' 'F'
    // 也就是要求 SOI 之后紧跟着 APP0/JFIF 段。而服务端的封面是 ffmpeg(Lavc)
    // 出的,SOI 之后是 COM 段(FF D8 FF FE ...) —— 完全合法的 JPEG,但嗅探
    // 不认,decoder_info 直接返回 INVALID,图就【不显示也不报错】。
    //
    // 所以在 SOI 后面补一段标准的 18 字节 JFIF APP0。JPEG 允许 APP0 出现在
    // 其它标记段之前,补上不影响原有的 COM 段,解码结果一模一样。
    // (LVGL 9.6 的嗅探宽松些,所以这个问题是降到 9.5 之后才冒出来的 ——
    //  而 9.5 是中文字库要的,见 idf_component.yml。)
    if (len > 4 && buf[0] == 0xFF && buf[1] == 0xD8 &&
        !(buf[2] == 0xFF && buf[3] == 0xE0)) {
        static const uint8_t APP0[18] = {
            0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
            0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00
        };
        uint8_t* fixed = (uint8_t*)heap_caps_malloc(len + sizeof(APP0), MALLOC_CAP_SPIRAM);
        if (fixed) {
            fixed[0] = 0xFF; fixed[1] = 0xD8;                   // SOI
            memcpy(fixed + 2, APP0, sizeof(APP0));              // 补 JFIF
            memcpy(fixed + 2 + sizeof(APP0), buf + 2, len - 2); // 原样接上剩下的
            free(buf);
            buf = fixed;
            len += sizeof(APP0);
        }
    }

    *out_len = len;
    return buf;
}

// 从 JPEG 头里读真实尺寸(扫到 SOFn 段)。
//
// ⚠️ 必须读,不能想当然。lv_image_dsc_t 的 header.w/h 是【我们告诉 LVGL 的】,
// LVGL 对 RAW 源完全信任这两个值(lv_tjpgd.c 的 decoder_info 直接照抄)。
// 之前这里写死成请求的边长,而服务端给的是 160 —— LVGL 以为图是 88x88,
// 于是只画了左上角那一块,看起来就是封面被放大裁掉了边。
bool jpeg_size(const uint8_t* d, int len, int* w, int* h)
{
    int i = 2;                                   // 跳过 SOI
    while (i + 9 < len) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        int seg = (d[i + 2] << 8) | d[i + 3];
        // SOFn:C0~CF,但 C4(DHT)/C8/CC 不是
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return *w > 0 && *h > 0;
        }
        if (seg <= 0) return false;
        i += 2 + seg;
    }
    return false;
}

void fill_dsc(lv_image_dsc_t* d, uint8_t* buf, int len, int px)
{
    *d = {};
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf    = LV_COLOR_FORMAT_RAW;   // 让解码器链去认格式
    int jw = px, jh = px;
    jpeg_size(buf, len, &jw, &jh);      // 读不出来就退回调用方给的边长
    d->header.w     = jw;
    d->header.h     = jh;
    d->data         = buf;
    d->data_size    = len;
}

// 播放页那张大图
bool fetch_cover(const char* key)
{
    int len = 0;
    uint8_t* buf = fetch_jpeg_url(nullptr, key, COVER_PX, &len);
    if (!buf) return false;
    if (g_cover_buf) free(g_cover_buf);
    g_cover_buf = buf;
    fill_dsc(&g_cover_dsc, g_cover_buf, len, COVER_PX);
    ESP_LOGI(TAG, "封面 %d 字节", len);
    return true;
}

// ── 列表缩略图 ────────────────────────────────────────────
// 列表里每行都要一张小封面 —— 这是"像个正经 app"和"一堆文字"的分界线。
// 缓存条目是固定数组,地址稳定,lv_image 可以直接指过去不用再拷。
// 抓取放后台任务里逐张来:一次 24 个 HTTP 请求会把界面卡死,而且顺序抓
// 正好是从上往下,用户看到的就是"图一张张浮出来",和手机上一样。
struct Thumb {
    uint8_t*       buf = nullptr;   // 原始 JPEG(转完就释放)
    lv_image_dsc_t dsc{};
    lv_draw_buf_t* rgb = nullptr;   // 解好的 RGB565,真正拿去画的是它
    bool           ready = false;
};
Thumb g_thumbs[MAX_SONGS];

// 把抓到的 JPEG 解成 RGB565,存回缓存。【必须在 LVGL 任务里调】。
//
// 为什么不直接把 JPEG 交给 lv_image:LVGL 每次重绘都会重解一遍,而且
// TJPGD 解出来是 RGB888,还要再混一道到 RGB565 缓冲 —— 封面墙上十几张图,
// 滑动时每帧全解一遍,卡得没法看。图片缓存也救不了:88x88 RGB888 一张 23KB,
// 三十张 700KB,超出缓存就开始颠簸。
// 解一次存成 RGB565,之后每帧只是整行 memcpy,正好走 Xtensa 汇编那条快路。
void decode_thumb(int i);

TaskHandle_t   g_thumb_task = nullptr;
volatile bool  g_thumb_stop = false;
// UI 重建代数。后台抓完图要回贴到行上,但那时界面可能已经被重建甚至拆掉了。
// 拿代数对一下,过期的更新直接丢掉,不去碰野指针。
volatile uint32_t g_ui_gen = 0;

// 实现见文件末尾(要用到 MusicApp::apply_thumb)

// ── UI ────────────────────────────────────────────────────
lv_obj_t* mk(lv_obj_t* p, const lv_font_t* f, uint32_t c, int x, int y, const char* s)
{
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

// 三个界面。默认进曲库;一旦开始播放就切到正在播放页 ——
// 音乐 app 的主界面本来就该是"现在在放什么",列表是去找歌时才用的。
enum class View { Library, NowPlaying, Search, Queue };

class MusicApp : public tdeck::App {
public:
    const char* name() const override   { return "Music"; }
    const lv_image_dsc_t* card_art() const override  { return &kCardArt; }
    const char* card_title() const override           { return "Music"; }
    const char* icon() const override   { return LV_SYMBOL_AUDIO; }
    uint32_t    accent() const override { return 0x55853A; }
    bool wants_raw_keys() const override { return view_ == View::Search; }

    // 快捷键一律进 s 那张表,不画在界面上(见 app.h 的 Shortcut 注释)。
    int shortcuts(const tdeck::Shortcut** out) const override
    {
        static const tdeck::Shortcut lib[] = {
            { "y / click", "play selected" },
            { "ball U/D",  "pick a song" },
        };
        static const tdeck::Shortcut now[] = {
            { "y / click", "play / pause" },
            { "ball L/R",  "prev / next" },
            { "ball down", "back to library" },
        };
        static const tdeck::Shortcut sea[] = {
            { "type",      "song name" },
            { "enter",     "search and play" },
            { "esc",       "back to list" },
        };
        static const tdeck::Shortcut que[] = {
            { "y / click", "play this one" },
            { "n / b",     "back to player" },
        };
        switch (view_) {
        case View::Library:    *out = lib; return 2;
        case View::NowPlaying: *out = now; return 3;
        case View::Search:     *out = sea; return 3;
        case View::Queue:      *out = que; return 2;
        }
        return 0;
    }

    // 卡片是入口不是挂件:一排静态均衡器条,靠形状认出这是音乐
    void render_card(lv_obj_t* card) override
    {
        const int W = 143, H = 94;
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1E2A16), LV_PART_MAIN);
        static const int hs[] = { 18, 34, 26, 46, 30, 54, 22, 40, 28, 16 };
        const int n = 10, bw = 8, gap = 5;
        int x0 = (W - (n * bw + (n - 1) * gap)) / 2;
        for (int i = 0; i < n; i++) {
            lv_obj_t* b = lv_obj_create(card);
            lv_obj_set_size(b, bw, hs[i]);
            lv_obj_set_pos(b, x0 + i * (bw + gap), 20 + (56 - hs[i]) / 2);
            lv_obj_set_style_radius(b, bw / 2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, lv_color_hex((i >= 3 && i <= 6) ? 0x55853A : 0x6E8B4F),
                                      LV_PART_MAIN);
            lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t* nm = lv_label_create(card);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nm, lv_color_hex(0x1B2117), LV_PART_MAIN);
        lv_label_set_text(nm, "Music");
        lv_obj_set_pos(nm, 12, H - 26);
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        self_ = this;

        tick_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<MusicApp*>(lv_timer_get_user_data(t))->on_tick();
        }, 500, this);

        if (g_state.n_songs == 0) {
            show_loading();
            xTaskCreate([](void* s) {
                fetch_list();
                lvgl_port_lock(0);
                auto* self = static_cast<MusicApp*>(s);
                if (self->root_) self->show_library();
                lvgl_port_unlock();
                vTaskDelete(nullptr);
            }, "music_list", 8192, this, 4, nullptr);
        } else {
            g_state.playing ? show_now() : show_library();
        }
    }

    void on_exit() override
    {
        if (tick_) { lv_timer_delete(tick_); tick_ = nullptr; }
        root_ = nullptr; self_ = nullptr;
        clear_refs();
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (view_) {
        case View::Search:     return search_input(ev);
        case View::Queue:      return queue_input(ev);
        case View::NowPlaying: return now_input(ev);
        case View::Library:    return lib_input(ev);
        }
        return false;
    }

    // 给 decode_thumb 用:解好之后把图贴到界面上(如果界面还在)。
    // 放在 public 段 —— 它是文件作用域函数的回调入口,不是内部实现细节。
    static void notify_thumb(int i)
    {
        if (self_ && self_->root_ && self_->view_ == View::Library) self_->apply_thumb(i);
    }

private:
    void clear_refs()
    {
        list_ = search_row_ = q_lbl_ = sr_ = nullptr;
        cover_ = title_ = artist_ = bar_fill_ = t_cur_ = t_tot_ = btn_play_ = nullptr;

        for (auto& r : rows_) r = nullptr;
        for (auto& m : play_mark_) m = nullptr;
        for (auto& x : row_img_) x = nullptr;
        for (auto& x : row_ph_)  x = nullptr;
        for (auto& x : shelf_)   x = nullptr;
        mini_ = nullptr;
    }

    static lv_obj_t* lbl(lv_obj_t* p, const lv_font_t* f, uint32_t c, int x, int y, const char* s)
    {
        lv_obj_t* l = lv_label_create(p);
        lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
        lv_label_set_text(l, s);
        lv_obj_set_pos(l, x, y);
        return l;
    }

    void show_loading()
    {
        lv_obj_clean(root_); clear_refs();
        lbl(root_, F20(), 0x1B2117, 14, 100, "loading...");
    }

    // ── 正在播放 ──────────────────────────────────────────
    // 封面 + 标题 + 进度 + 传输控件。深色底 —— 音乐播放器几乎都是深色,
    // 封面在深底上才显得亮;而且这一屏几乎没有留白,漏光没有显形的余地。
    void show_now()
    {
        view_ = View::NowPlaying;
        lv_obj_clean(root_); clear_refs();

        // 封面。没拉到就画一块占位,不要留个空洞
        cover_ = lv_image_create(root_);
        lv_obj_set_pos(cover_, 16, 44);
        if (g_cover_buf) {
            lv_image_set_src(cover_, &g_cover_dsc);
        } else {
            lv_obj_delete(cover_);
            cover_ = lv_obj_create(root_);
            lv_obj_set_size(cover_, 120, 120);
            lv_obj_set_pos(cover_, 16, 44);
            lv_obj_set_style_bg_color(cover_, lv_color_hex(0xE8EEE0), LV_PART_MAIN);
            lv_obj_set_style_radius(cover_, 10, LV_PART_MAIN);
            lv_obj_set_style_border_width(cover_, 0, LV_PART_MAIN);
            lv_obj_remove_flag(cover_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* n = lbl(cover_, &lv_font_montserrat_28, 0x55853A, 0, 0, LV_SYMBOL_AUDIO);
            lv_obj_center(n);
        }

        const int RX = 152, RW = 154;
        title_ = lbl(root_, F20(), 0x141A10, RX, 48, g_state.title);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title_, RW);
        // 定宽不够,还要定高。LONG_DOT 在高度不受限时会先换行再省略,
        // 长标题会一路长下去把艺术家和进度条压在底下。
        lv_obj_set_height(title_, CJK_LINE_H * 2);   // 恰好两行

        artist_ = lbl(root_, FS(), 0x6A7360, RX, 106, g_state.artist);
        lv_label_set_long_mode(artist_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(artist_, RW);
        lv_obj_set_height(artist_, SMALL_LH());

        lv_obj_t* track = lv_obj_create(root_);
        lv_obj_set_size(track, RW, 4);
        lv_obj_set_pos(track, RX, 134);
        lv_obj_set_style_bg_color(track, lv_color_hex(0xDDE3D4), LV_PART_MAIN);
        lv_obj_set_style_radius(track, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
        lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

        bar_fill_ = lv_obj_create(track);
        lv_obj_set_size(bar_fill_, 0, 4);
        lv_obj_set_pos(bar_fill_, 0, 0);
        lv_obj_set_style_bg_color(bar_fill_, lv_color_hex(0x55853A), LV_PART_MAIN);
        lv_obj_set_style_radius(bar_fill_, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar_fill_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar_fill_, 0, LV_PART_MAIN);

        t_cur_ = lbl(root_, &lv_font_montserrat_14, 0x6A7360, RX, 144, "0:00");
        t_tot_ = lbl(root_, &lv_font_montserrat_14, 0x6A7360, RX + RW - 34, 144, "0:00");

        // ── 左上角:返回 / 曲库 ────────────────────────────
        // 右上角【不放东西】:那一角背光漏光最重(见 docs/hardware.md),
        // 浅色内容摆上去会被红光吃掉。
        // ⚠️「返回」回【曲库】,不是退出整个 app。
        // 之前这里调的是 launcher_back(),一按就直接跳回桌面 ——
        // 从播放页出来本该退一层,不是退到底。
        mk_pill(12, 10, LV_SYMBOL_LEFT, "返回", [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->show_library();
        });
        // 「播放列表」= 接下来会播的队列,和"回到曲库"是两码事。
        mk_pill(80, 10, LV_SYMBOL_LIST, "播放列表", [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->show_queue();
        });

        // ── 底部控制条 ────────────────────────────────────
        // 封面占到 y=164,所有按钮必须整体落在它【下面】。之前按钮 cy 是
        // 178/182、直径 56,上沿到 150,被封面压住了一半 —— 一行统一 cy=196,
        // 最大的那颗上沿也只到 168,不会再撞。
        //
        // 音量胶囊靠左,传输键组在【剩下的空间里】居中(不是全屏居中):
        // 胶囊右沿 76,屏宽 320,所以组心在 198,左右各留 48。全屏居中的话
        // 右边会空出 86px,看着像掉了一个按钮。
        // 三颗传输键的【中心距】原来只有 52,而直径是 44 和 56 ——
        // 边缘只剩 2px,看着像挤在一起。拉到 64:56/2 + 44/2 = 50,留 14px 空隙。
        constexpr int ROW_Y = 196;
        constexpr int GAP   = 64;
        mk_vol_pill(40, ROW_Y);
        mk_btn(SCR_W / 2 + 30 - GAP, ROW_Y, 44, LV_SYMBOL_PREV, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->skip(-1);
        });
        btn_play_ = mk_btn(SCR_W / 2 + 30, ROW_Y, 56, LV_SYMBOL_PAUSE, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->toggle();
        });
        mk_btn(SCR_W / 2 + 30 + GAP, ROW_Y, 44, LV_SYMBOL_NEXT, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->skip(1);
        });

        on_tick();
    }

    // 图标 + 文字的小药丸按钮。图标用 LVGL 的 symbol 字形,不用 "<" / "=" 这类
    // 字符凑合 —— 字符在 20px 下辨识度差,而且和正文混在一起不像可按的东西。
    lv_obj_t* mk_pill(int x, int y, const char* sym, const char* text, lv_event_cb_t cb)
    {
        lv_obj_t* b = lv_obj_create(root_);
        lv_obj_set_height(b, 26);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, 13, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xEDF1E7), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(b, 5, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        // flex + 宽度自适应内容:文字长度不一样("Back" vs "Playlist"),
        // 写死宽度不是留白太多就是把字挤掉。
        lv_obj_set_layout(b, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_width(b, LV_SIZE_CONTENT);

        lv_obj_t* i = lv_label_create(b);
        lv_obj_set_style_text_font(i, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(i, lv_color_hex(0x55853A), LV_PART_MAIN);
        lv_label_set_text(i, sym);

        // ⚠️ 文字用中文字体,不能用 Montserrat —— 药丸上写的是"返回""播放列表",
        // Montserrat 没有汉字字形,出来是一排方框(图标那一层仍然是 Montserrat,
        // 因为 LV_SYMBOL_* 是 FontAwesome 私用区码位,中文字库里没有)。
        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, FS(), LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0x3D4636), LV_PART_MAIN);
        lv_label_set_text(l, text);
        return b;
    }

    // 音量做成一颗胶囊,左右各一半:− 和 + 是一对,分成两颗独立圆钮会让它们
    // 看起来和 prev/next 同级,实际不是。中间一道细分隔线提示"这里可以按两边"。
    void mk_vol_pill(int cx, int cy)
    {
        constexpr int PW = 68, PH = 38;
        lv_obj_t* pill = lv_obj_create(root_);
        lv_obj_set_size(pill, PW, PH);
        lv_obj_set_pos(pill, cx - PW / 2, cy - PH / 2);
        lv_obj_set_style_radius(pill, PH / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0xEDF1E7), LV_PART_MAIN);
        lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pill, 0, LV_PART_MAIN);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* div = lv_obj_create(pill);
        lv_obj_set_size(div, 1, 18);
        lv_obj_set_pos(div, PW / 2, (PH - 18) / 2);
        lv_obj_set_style_bg_color(div, lv_color_hex(0xD3DBC8), LV_PART_MAIN);
        lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN);

        auto half = [&](int x, const char* sym, int delta) {
            lv_obj_t* h = lv_obj_create(pill);
            lv_obj_set_size(h, PW / 2, PH);
            lv_obj_set_pos(h, x, 0);
            lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(h, 0, LV_PART_MAIN);
            lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(h, (void*)(intptr_t)delta);
            lv_obj_add_event_cb(h, [](lv_event_t* e) {
                int d = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                tdeck::volume::step(d);
            }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* l = lv_label_create(h);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, lv_color_hex(0x55853A), LV_PART_MAIN);
            lv_label_set_text(l, sym);
            lv_obj_center(l);
        };
        half(0,      LV_SYMBOL_MINUS, -1);
        half(PW / 2, LV_SYMBOL_PLUS,  +1);
    }

    lv_obj_t* mk_btn(int cx, int cy, int d, const char* sym, lv_event_cb_t cb)
    {
        lv_obj_t* b = lv_obj_create(root_);
        lv_obj_set_size(b, d, d);
        lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(d >= 56 ? 0x55853A : 0xEDF1E7), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* s = lbl(b, &lv_font_montserrat_20, d >= 56 ? 0xFFFFFF : 0x55853A, 0, 0, sym);
        lv_obj_center(s);
        return b;
    }

    void on_tick()
    {
        if (view_ != View::NowPlaying || !bar_fill_) {
            if (view_ == View::Library) paint_playing();
            return;
        }
        int dur = g_state.dur_s > 0 ? g_state.dur_s : 1;
        int w = 154 * g_state.pos_s / dur;
        if (w > 154) w = 154;
        lv_obj_set_width(bar_fill_, w);
        char b[16];
        snprintf(b, sizeof(b), "%d:%02d", g_state.pos_s / 60, g_state.pos_s % 60);
        lv_label_set_text(t_cur_, b);
        snprintf(b, sizeof(b), "%d:%02d", g_state.dur_s / 60, g_state.dur_s % 60);
        lv_label_set_text(t_tot_, b);
        if (btn_play_) {
            lv_obj_t* s = lv_obj_get_child(btn_play_, 0);
            if (s) lv_label_set_text(s, g_state.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }

    void toggle()
    {
        if (g_state.playing) { stop_playback(); }
        else if (playing_idx_ >= 0) { play_index(playing_idx_); }
        on_tick();
    }

    void skip(int d)
    {
        if (g_state.n_songs == 0) return;
        int n = playing_idx_ < 0 ? 0 : (playing_idx_ + d + g_state.n_songs) % g_state.n_songs;
        stop_playback();
        play_index(n);
        show_now();
    }

    void play_index(int i)
    {
        if (i < 0 || i >= g_state.n_songs) return;
        char u[400];
        snprintf(u, sizeof(u), MUSIC_BASE_URL "/audio?f=%s.opus&key=" MUSIC_TOKEN, g_songs[i].key);
        start_play(u, g_songs[i].title, g_songs[i].artist, g_songs[i].dur, g_songs[i].key);
        playing_idx_ = i;
    }

    bool now_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Enter: toggle();          return true;
        case tdeck::Key::Left:  skip(-1);          return true;
        case tdeck::Key::Right: skip(1);           return true;
        case tdeck::Key::Down:  show_library();    return true;
        default: return false;
        }
    }

    // ── 首页:封面墙 ──────────────────────────────────────
    //
    // 照手机音乐 app 的首页来:竖着滚分区,每个分区里横着滚一排方形封面,
    // 封面下面两行字(歌名 / 歌手)。服务端 /api/reco?kind=mine 一次就把
    // 分区结构给全了(recent / most / cached),不用我们自己攒。
    //
    // 尺寸是被这块屏逼出来的:中文字库只有 20px 一个字号、行高 26,
    // 两行字就要 52px。封面取 88 —— 再大的话一屏连一个分区都放不下,
    // 再小就撑不起"封面墙"的样子。一屏正好看到一个完整分区加下一个的标题,
    // 和手机上滚动的节奏是一样的。
    static constexpr int HDR_H     = 34;
    static constexpr int SEC_TTL_H = 30;              // 分区标题
    static constexpr int SEC_GAP   = 16;              // 分区之间的呼吸
    static constexpr int COVER     = 88;
    static constexpr int TILE_GAP  = 2;
    // 格子四周都比封面大出 TILE_PAD:选中时的 outline 画在封面【外侧】,
    // 格子要是正好贴着封面,这一圈就被父容器裁掉。
    // ⚠️ 上边也要留 —— 只留左右的话,封面【上方】那一段边框照样没了。
    static constexpr int TILE_PAD  = 3;   // 刚好容下 3px 的选中框
    static constexpr int TILE_W    = COVER + TILE_PAD * 2;
    int tile_h() const { return TILE_PAD + COVER + 4 + SMALL_LH() * 2; }
    int section_h() const { return SEC_TTL_H + tile_h() + SEC_GAP; }
    static constexpr int SIDE      = 12;              // 左右留白
    static constexpr int MINI_H    = 48;

    void show_library()
    {
        view_ = View::Library;
        lv_obj_clean(root_); clear_refs();
        g_ui_gen++;                       // 之前那批封面回贴作废

        build_header();

        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W, SCR_H - HDR_H);
        lv_obj_set_pos(list_, 0, HDR_H);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        int y = 0;
        for (int si = 0; si < SEC_N; si++) {
            if (g_sec[si].count == 0) continue;
            build_section(si, y);
            y += section_h();
        }
        // 底部留出浮层 mini player 的高度,免得最后一个分区被压住
        if (g_state.playing) {
            lv_obj_t* pad = lv_obj_create(list_);
            lv_obj_set_size(pad, 2, MINI_H);
            lv_obj_set_pos(pad, 0, y);
            lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
        }

        if (g_state.playing) build_mini();
        paint(); paint_playing();
        start_thumbs();
    }

    void build_header()
    {
        lv_obj_t* h = lv_obj_create(root_);
        lv_obj_set_size(h, SCR_W, HDR_H);
        lv_obj_set_pos(h, 0, 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(h, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(h, 0, LV_PART_MAIN);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);

        lbl(h, F20(), C_TEXT, SIDE, (HDR_H - CJK_LINE_H) / 2, "音乐");

        // 搜索:LVGL 内建的 FontAwesome 子集里没有放大镜,拿加号凑会读成"添加"。
        // 这台机器有实体键盘,键盘图标反而更准 —— 意思就是"打字搜"。
        search_row_ = lv_obj_create(h);
        lv_obj_set_size(search_row_, 28, 28);
        lv_obj_set_pos(search_row_, SCR_W - 40, 3);
        lv_obj_set_style_radius(search_row_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(search_row_, lv_color_hex(0xEDF1E7), LV_PART_MAIN);
        lv_obj_set_style_border_width(search_row_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(search_row_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(search_row_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(search_row_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(search_row_, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->enter_search(0);
        }, LV_EVENT_CLICKED, this);
        // LVGL 内建的 FontAwesome 子集里【没有放大镜】。之前拿键盘图标顶替,
        // 但那读起来是"打字"不是"搜索"。这里用两个基本图元拼一个:
        // 一个只有描边的圆 + 一条斜线手柄。lv_line 可以直接给两点,
        // 不需要旋转变换(9.1 里旋转属性的名字和后来版本不一样,少碰为妙)。
        lv_obj_t* ring = lv_obj_create(search_row_);
        lv_obj_set_size(ring, 13, 13);
        lv_obj_set_pos(ring, 5, 5);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

        static const lv_point_precise_t handle[] = { {0, 0}, {5, 5} };
        lv_obj_t* ln = lv_line_create(search_row_);
        lv_line_set_points(ln, handle, 2);
        lv_obj_set_pos(ln, 17, 17);
        lv_obj_set_style_line_width(ln, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(ln, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
    }

    void build_section(int si, int y)
    {
        lbl(list_, F20(), C_TEXT, SIDE, y + (SEC_TTL_H - CJK_LINE_H) / 2, g_sec[si].title);

        // 横向滚动的封面排。每个分区一个独立的滚动容器,
        // 互不影响 —— 和手机上一样,滑哪一排动哪一排。
        lv_obj_t* shelf = lv_obj_create(list_);
        lv_obj_set_size(shelf, SCR_W, tile_h());
        lv_obj_set_pos(shelf, 0, y + SEC_TTL_H);
        lv_obj_set_style_bg_opa(shelf, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(shelf, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(shelf, 0, LV_PART_MAIN);
        // ⚠️ 左右留白要做成【容器的 padding】,不能靠"第一个格子往右挪一点"。
        // 挪格子的话这段留白属于内容,一横向滚动就跟着滚没了,
        // 而下面没滚过的那排还留着 —— 两排左边对不齐,很明显。
        // 做成 padding 之后它不属于内容,滚到头始终留得住。
        // 减掉 TILE_PAD 是为了让封面(格子里再内缩 TILE_PAD)和分区标题左对齐。
        lv_obj_set_style_pad_all(shelf, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(shelf, SIDE - TILE_PAD, LV_PART_MAIN);
        lv_obj_set_style_pad_right(shelf, SIDE - TILE_PAD, LV_PART_MAIN);
        lv_obj_set_scroll_dir(shelf, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(shelf, LV_SCROLLBAR_MODE_OFF);
        shelf_[si] = shelf;

        for (int j = 0; j < g_sec[si].count; j++)
            build_tile(shelf, g_sec[si].start + j, j * (TILE_W + TILE_GAP));
    }

    void build_tile(lv_obj_t* shelf, int i, int x)
    {
        lv_obj_t* c = lv_obj_create(shelf);
        lv_obj_set_size(c, TILE_W, tile_h());
        lv_obj_set_pos(c, x, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(c, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(c, (void*)(intptr_t)i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);   // 横滑手势要能穿过去
        lv_obj_add_event_cb(c, [](lv_event_t* e) {
            auto* s = static_cast<MusicApp*>(lv_event_get_user_data(e));
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
            s->stop_and_play(idx);
        }, LV_EVENT_CLICKED, this);

        // 封面位先摆底色,图抓到了再盖上去 —— 尺寸从一开始就定死,
        // 免得图陆续到达时整排一跳一跳地重排。
        lv_obj_t* ph = lv_obj_create(c);
        lv_obj_set_size(ph, COVER, COVER);
        lv_obj_set_pos(ph, TILE_PAD, TILE_PAD);
        lv_obj_set_style_bg_color(ph, lv_color_hex(0xE8EEE0), LV_PART_MAIN);
        lv_obj_set_style_radius(ph, COVER_R, LV_PART_MAIN);
        lv_obj_set_style_border_width(ph, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ph, 0, LV_PART_MAIN);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* nt = lbl(ph, FICON(), 0xB6C2A6, 0, 0, LV_SYMBOL_AUDIO);
        lv_obj_center(nt);
        row_ph_[i] = ph;

        lv_obj_t* im = lv_image_create(c);
        lv_obj_set_pos(im, TILE_PAD, TILE_PAD);
        lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(im, COVER_R, LV_PART_MAIN);   // outline 跟着这个半径走
        row_img_[i] = im;

        // 正在播的那首:封面右下角压一个小三角
        play_mark_[i] = lbl(c, FICON(), 0xFFFFFF, TILE_PAD + COVER - 18, TILE_PAD + COVER - 20, LV_SYMBOL_PLAY);
        lv_obj_add_flag(play_mark_[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* tl = lbl(c, FS(), C_TEXT, 0, TILE_PAD + COVER + 4, g_songs[i].title);
        lv_obj_set_size(tl, TILE_W, SMALL_LH());
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

        lv_obj_t* al = lbl(c, FS(), 0x9AA48C, 0, TILE_PAD + COVER + 4 + SMALL_LH(),
                           g_songs[i].artist[0] ? g_songs[i].artist : "-");
        lv_obj_set_size(al, TILE_W, SMALL_LH());
        lv_label_set_long_mode(al, LV_LABEL_LONG_DOT);

        rows_[i] = c;
        if (g_thumbs[i].ready) apply_thumb(i);
    }

    // 底部浮着的 mini player。手机音乐 app 的标配:翻歌的时候当前这首
    // 永远在手边,不用退回播放页。做成【浮层】压在内容上,不占版面高度。
    void build_mini()
    {
        lv_obj_t* m = lv_obj_create(root_);
        lv_obj_set_size(m, SCR_W - SIDE * 2, MINI_H - 8);
        lv_obj_set_pos(m, SIDE, SCR_H - MINI_H);
        lv_obj_set_style_bg_color(m, lv_color_hex(0xF4F7F0), LV_PART_MAIN);
        lv_obj_set_style_radius(m, (MINI_H - 8) / 2, LV_PART_MAIN);   // 胶囊
        lv_obj_set_style_border_width(m, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(m, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(m, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(m, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_pad_all(m, 0, LV_PART_MAIN);
        lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(m, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->show_now();
        }, LV_EVENT_CLICKED, this);

        // 封面:优先用列表那张解好的缩略图;没有就退回播放页那张 120px 大图
        // (start_play 一定会抓它)。从搜索直接播的歌不在歌曲池里、
        // playing_idx_ 是 -1,只有这条退路能显示封面 —— 之前少了它,
        // 悬浮框就是空的。
        const int MP = 30;
        const void* mini_src = nullptr;
        int mini_px = THUMB_PX;
        if (playing_idx_ >= 0 && playing_idx_ < MAX_SONGS && g_thumbs[playing_idx_].ready) {
            mini_src = g_thumbs[playing_idx_].rgb;
        } else if (g_cover_buf) {
            mini_src = &g_cover_dsc;
            mini_px  = COVER_PX;
        }
        if (mini_src) {
            lv_obj_t* im = lv_image_create(m);
            lv_image_set_src(im, mini_src);
            // ⚠️ lv_image_set_scale 是【绕轴心】缩的,轴心默认在图正中。
            // 不把轴心挪到左上角的话,缩完内容会落到控件外面被裁光,
            // 看上去就是"图没显示",而且不报错。
            lv_image_set_pivot(im, 0, 0);
            lv_image_set_scale(im, 256 * MP / mini_px);
            lv_obj_set_size(im, MP, MP);
            lv_obj_set_pos(im, 5, (MINI_H - 8 - MP) / 2);
            lv_obj_set_style_radius(im, 5, LV_PART_MAIN);
            lv_obj_set_style_clip_corner(im, true, LV_PART_MAIN);
        }

        char line[160];
        if (g_state.artist[0])
            snprintf(line, sizeof(line), "%s · %s", g_state.title, g_state.artist);
        else
            snprintf(line, sizeof(line), "%s", g_state.title);
        lv_obj_t* l = lbl(m, F16(), C_TEXT, 42, (MINI_H - 8 - CJK_LINE_H) / 2, line);
        lv_obj_set_size(l, SCR_W - SIDE * 2 - 42 - 44, CJK_LINE_H);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

        lv_obj_t* b = lv_obj_create(m);
        lv_obj_set_size(b, 30, 30);
        lv_obj_set_pos(b, SCR_W - SIDE * 2 - 36, (MINI_H - 8 - 30) / 2);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            lv_event_stop_bubbling(e);   // 别连带触发 mini 条的"进播放页"
            static_cast<MusicApp*>(lv_event_get_user_data(e))->toggle();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* s = lbl(b, FICON(), 0xFFFFFF, 0, 0,
                          g_state.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_center(s);
        mini_ = m;
    }

    void apply_thumb(int i)
    {
        if (!row_img_[i] || !g_thumbs[i].ready) return;
        lv_image_set_src(row_img_[i], g_thumbs[i].rgb);
        lv_obj_remove_flag(row_img_[i], LV_OBJ_FLAG_HIDDEN);
        if (row_ph_[i]) lv_obj_add_flag(row_ph_[i], LV_OBJ_FLAG_HIDDEN);
    }

    void start_thumbs()
    {
        if (g_thumb_task) return;          // 已经在抓了
        g_thumb_stop = false;
        BaseType_t ok = xTaskCreate([](void*) {
            uint32_t gen = g_ui_gen;
            for (int i = 0; i < g_state.n_songs && !g_thumb_stop; i++) {
                if (!g_thumbs[i].ready) {
                    // 同一首歌会同时出现在"最近播放"和"已缓存"里。
                    // 先在前面找有没有抓过同一个 key,有就直接共用,
                    // 省掉一次 HTTP 和一份 PSRAM。
                    int dup = -1;
                    for (int j = 0; j < i; j++)
                        if (g_thumbs[j].ready && !strcmp(g_songs[j].key, g_songs[i].key)) { dup = j; break; }
                    if (dup >= 0) {
                        g_thumbs[i].rgb = g_thumbs[dup].rgb;   // 共用同一块解好的位图
                        g_thumbs[i].buf = nullptr;             // 不是所有者,别 free
                        g_thumbs[i].ready = true;
                    } else {
                        int len = 0;
                        uint8_t* b = fetch_jpeg_url(g_songs[i].cover, g_songs[i].key, THUMB_PX, &len);
                        if (!b) continue;
                        fill_dsc(&g_thumbs[i].dsc, b, len, THUMB_PX);
                        g_thumbs[i].buf = b;

                        // ⚠️ 解码【交给 LVGL 任务做】,不在这个任务里做。
                        //
                        // 解 JPEG 要走 LVGL 的绘制管线,栈很深(16K 起)。
                        // 如果在抓取任务里做,这个任务就得也开 16K,而它和
                        // 20K 的播放任务同时活着 —— 内部 RAM 实测被压到只剩
                        // 6.5KB,一崩一个准。
                        // lv_async_call 把活儿排到 LVGL 任务上,那边本来就有 16K。
                        lv_async_call([](void* arg) {
                            int idx = (int)(intptr_t)arg;
                            decode_thumb(idx);
                        }, (void*)(intptr_t)i);
                    }
                }
                // 回贴到界面上。代数对不上说明界面已经重建过(或者 app
                // 已经退出),这时候去碰那些 lv_obj_t* 就是野指针。
                // 贴图这一步由 decode_thumb 解完后自己做(它就在 LVGL 任务里),
                // 这里只负责把 JPEG 抓回来。
                (void)gen;
            }
            g_thumb_task = nullptr;
            vTaskDelete(nullptr);
        }, "music_thumb", 6144, nullptr, 3, &g_thumb_task);   // 只做 HTTP,解码在 LVGL 任务里
        if (ok != pdPASS) {
            g_thumb_task = nullptr;
            ESP_LOGW(TAG, "缩略图任务起不来,内部 RAM 剩 %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }

    // ── 播放列表(接下来播放)────────────────────────────
    //
    // 这和"回到曲库"是两码事:曲库是【你去挑】,播放列表是【系统排好接下来放什么】。
    // 现在的排法就是当前这首往后顺着走 —— skip(+1) 走的也是这条线,
    // 两者必须一致,否则"下一首"按钮和这张表会对不上。
    // (以后要做随机/循环,改这里和 skip() 同一处即可。)
    void show_queue()
    {
        view_ = View::Queue;
        lv_obj_clean(root_); clear_refs();
        g_ui_gen++;

        mk_pill(12, 10, LV_SYMBOL_LEFT, "返回", [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->show_now();
        });
        lbl(root_, F20(), C_TEXT, 96, 10, "接下来播放");

        lv_obj_t* box = lv_obj_create(root_);
        lv_obj_set_size(box, SCR_W, SCR_H - 46);
        lv_obj_set_pos(box, 0, 46);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(box, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
        list_ = box;

        const int RH = 44, TH = 34;
        int row = 0;
        for (int i = playing_idx_ + 1; i < g_state.n_songs && row < 12; i++, row++) {
            lv_obj_t* r = lv_obj_create(box);
            lv_obj_set_size(r, SCR_W, RH);
            lv_obj_set_pos(r, 0, row * RH);
            lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(r, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_user_data(r, (void*)(intptr_t)i);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_event_cb(r, [](lv_event_t* e) {
                auto* s = static_cast<MusicApp*>(lv_event_get_user_data(e));
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                s->stop_and_play(idx);
            }, LV_EVENT_CLICKED, this);

            // 队列里的封面用已有的缩略图缩一下,不额外下载
            if (g_thumbs[i].ready) {
                lv_obj_t* im = lv_image_create(r);
                lv_image_set_src(im, g_thumbs[i].rgb);
                lv_image_set_pivot(im, 0, 0);      // 见 build_mini 里的说明
                lv_image_set_scale(im, 256 * TH / THUMB_PX);
                lv_obj_set_size(im, TH, TH);
                lv_obj_set_pos(im, 12, (RH - TH) / 2);
                lv_obj_set_style_radius(im, 5, LV_PART_MAIN);
                lv_obj_set_style_clip_corner(im, true, LV_PART_MAIN);
            }

            char line[160];
            if (g_songs[i].artist[0])
                snprintf(line, sizeof(line), "%s · %s", g_songs[i].title, g_songs[i].artist);
            else
                snprintf(line, sizeof(line), "%s", g_songs[i].title);
            lv_obj_t* l = lbl(r, FS(), C_TEXT, 56, (RH - SMALL_LH()) / 2, line);
            lv_obj_set_size(l, SCR_W - 56 - 14, SMALL_LH());
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        }
        if (row == 0)
            lbl(root_, FS(), C_MUTE, 14, 70, "后面没有了");
    }

    bool queue_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { show_now(); return true; }
        return false;
    }

    void stop_and_play(int i)
    {
        stop_playback();
        play_index(i);
        show_now();
    }

    void paint()
    {
        if (search_row_) {
            bool on = (sel_ < 0);
            lv_obj_set_style_bg_color(search_row_,
                lv_color_hex(on ? C_ACCENT : 0xEDF1E7), LV_PART_MAIN);
            // 放大镜是圆环 + 线两个对象,选中时一起翻成白色
            uint32_t fg = on ? 0xFFFFFF : C_ACCENT;
            if (lv_obj_t* ring = lv_obj_get_child(search_row_, 0))
                lv_obj_set_style_border_color(ring, lv_color_hex(fg), LV_PART_MAIN);
            if (lv_obj_t* ln = lv_obj_get_child(search_row_, 1))
                lv_obj_set_style_line_color(ln, lv_color_hex(fg), LV_PART_MAIN);
        }
        // 选中态用 【outline】 而不是 border。
        // border 是画在对象【内部】的,一加上去内容就被挤进来、封面整个位移,
        // 选中/取消之间图会跳一下,很难看。outline 画在对象外侧,
        // 不参与布局也不影响内容,只是外面多一圈。
        for (int i = 0; i < g_state.n_songs; i++) {
            bool on = (i == sel_);
            for (lv_obj_t* o : { row_ph_[i], row_img_[i] }) {
                if (!o) continue;
                lv_obj_set_style_outline_width(o, on ? 3 : 0, LV_PART_MAIN);
                lv_obj_set_style_outline_pad(o, 0, LV_PART_MAIN);  // 贴着封面,不留缝
                lv_obj_set_style_outline_color(o, lv_color_hex(C_ACCENT), LV_PART_MAIN);
            }
        }
        // ⚠️ 必须用 recursive 版本。lv_obj_scroll_to_view 只滚对象的【直接父容器】,
        // 而这里是两层:格子在横向的"排"里,排又在竖向的列表里。只滚一层的话
        // 横向能跟上、竖向不动 —— 轨迹球往下选到第二排就跑到屏幕外面看不见了。
        if (sel_ >= 0 && rows_[sel_]) lv_obj_scroll_to_view_recursive(rows_[sel_], LV_ANIM_ON);
    }

    void paint_playing()
    {
        for (int i = 0; i < g_state.n_songs; i++) {
            if (!play_mark_[i]) continue;
            bool on = (i == playing_idx_ && g_state.playing);
            if (on) lv_obj_remove_flag(play_mark_[i], LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(play_mark_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 选中的歌在哪个分区、是该分区的第几个
    void sel_pos(int idx, int* sec, int* off) const
    {
        for (int si = 0; si < SEC_N; si++) {
            if (idx >= g_sec[si].start && idx < g_sec[si].start + g_sec[si].count) {
                *sec = si; *off = idx - g_sec[si].start; return;
            }
        }
        *sec = 0; *off = 0;
    }

    bool lib_input(const tdeck::InputEvent& ev)
    {
        if (g_state.n_songs == 0) {
            if (ev.key == tdeck::Key::Enter) { enter_search(0); return true; }
            return false;
        }
        // 封面墙是二维的:左右在同一排里走,上下换排 —— 和手机上一样。
        // sel_ 仍然是"歌曲池里的下标",换排时按【当前排的位置】映射到
        // 目标排的同一位置,越界就贴到那一排的末尾。
        int sec = 0, off = 0;
        sel_pos(sel_ < 0 ? 0 : sel_, &sec, &off);

        auto goto_sec = [&](int ns) {
            while (ns >= 0 && ns < SEC_N && g_sec[ns].count == 0) ns += (ns > sec ? 1 : -1);
            if (ns < 0 || ns >= SEC_N) return;
            int o = off < g_sec[ns].count ? off : g_sec[ns].count - 1;
            sel_ = g_sec[ns].start + o;
            paint();
        };

        switch (ev.key) {
        case tdeck::Key::Left:
            if (sel_ < 0) return true;
            if (off > 0) { sel_--; paint(); }
            return true;
        case tdeck::Key::Right:
            if (sel_ < 0) { sel_ = 0; paint(); return true; }
            if (off < g_sec[sec].count - 1) { sel_++; paint(); }
            return true;
        case tdeck::Key::Up:
            // 第一排再往上就选到标题栏的搜索钮
            if (sel_ < 0) return true;
            if (sec == 0) { sel_ = -1; paint(); }
            else          goto_sec(sec - 1);
            return true;
        case tdeck::Key::Down:
            if (sel_ < 0) { sel_ = g_sec[0].start; paint(); return true; }
            goto_sec(sec + 1);
            return true;
        case tdeck::Key::Enter:
            if (sel_ < 0) { enter_search(0); return true; }
            stop_and_play(sel_);
            return true;
        default: return false;
        }
    }

    // ── 搜索 ──────────────────────────────────────────────
    void enter_search(char first)
    {
        view_ = View::Search;
        q_len_ = 0; q_[0] = 0;
        if (first) { q_[q_len_++] = first; q_[q_len_] = 0; }
        lv_obj_clean(root_); clear_refs();
        lbl(root_, &lv_font_montserrat_14, 0x8A9480, 16, 14, "SEARCH");

        lv_obj_t* box = lv_obj_create(root_);
        lv_obj_set_size(box, SCR_W - 28, 46);
        lv_obj_set_pos(box, 14, 44);
        lv_obj_set_style_bg_color(box, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
        lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, lv_color_hex(0x55853A), LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        q_lbl_ = lbl(box, F20(), 0x1B2117, 12, 11, q_);

        // 提示只说"要干什么",不说"按哪个键" —— 快捷键统一进 s 那张表。
        sr_ = lbl(root_, F16(), 0x8A9480, 16, 104, "输入歌名");
    }

    bool search_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { show_library(); return true; }
        if (ev.key == tdeck::Key::Enter) {
            if (!q_len_) return true;
            lv_label_set_text(sr_, "searching...");
            static char qcopy[128];
            strncpy(qcopy, q_, sizeof(qcopy) - 1);
            xTaskCreate([](void*) {
                bool ok = search_and_play(qcopy);
                lvgl_port_lock(0);
                if (self_ && self_->root_) {
                    if (ok) self_->show_now();
                    else if (self_->sr_) lv_label_set_text(self_->sr_, "not found");
                }
                lvgl_port_unlock();
                vTaskDelete(nullptr);
            }, "music_search", 8192, nullptr, 4, nullptr);
            return true;
        }
        if (ev.key == tdeck::Key::Char) {
            if (ev.ch == 8) { if (q_len_) q_[--q_len_] = 0; }
            else if (ev.ch >= 32 && ev.ch < 127 && q_len_ < (int)sizeof(q_) - 1) {
                q_[q_len_++] = ev.ch; q_[q_len_] = 0;
            }
            lv_label_set_text(q_lbl_, q_);
            return true;
        }
        return false;
    }

    lv_obj_t* root_ = nullptr, *list_ = nullptr, *search_row_ = nullptr;
    lv_obj_t* q_lbl_ = nullptr, *sr_ = nullptr;
    lv_obj_t* cover_ = nullptr, *title_ = nullptr, *artist_ = nullptr;
    lv_obj_t* bar_fill_ = nullptr, *t_cur_ = nullptr, *t_tot_ = nullptr, *btn_play_ = nullptr;
    lv_obj_t* rows_[MAX_SONGS] = {};
    lv_obj_t* row_img_[MAX_SONGS] = {};
    lv_obj_t* row_ph_[MAX_SONGS]  = {};
    lv_obj_t* shelf_[SEC_N] = {};
    lv_obj_t* mini_ = nullptr;
    lv_obj_t* play_mark_[MAX_SONGS] = {};
    lv_timer_t* tick_ = nullptr;
    char q_[128] = {};
    int  q_len_ = 0, sel_ = 0, playing_idx_ = -1;
    View view_ = View::Library;
    static MusicApp* self_;
};

MusicApp* MusicApp::self_ = nullptr;

// 把快照四角抹成背景色,做出圆角。
//
// ⚠️ 不用 LVGL 的 radius + clip_corner:clip_corner 裁的是【子对象】,
// 图片本身是画在对象自己身上的,裁不到 —— 实测只有选中那张看着是圆角,
// 那其实是外面那圈 outline 的圆角造成的错觉,图还是方的。
// 快照是 RGB565、没有 alpha,所以直接把角上的像素写成背景色(白),
// 页面底色也是白,看起来就是圆角。边缘做一档半透混合,免得锯齿太硬。
void round_corners(lv_draw_buf_t* buf, int r, uint16_t bg)
{
    if (!buf || r <= 0) return;
    const int w = (int)buf->header.w, h = (int)buf->header.h;
    const int stride = (int)buf->header.stride;
    uint8_t* base = buf->data;
    const int bg_r = (bg >> 11) & 31, bg_g = (bg >> 5) & 63, bg_b = bg & 31;

    for (int cy = 0; cy < 2; cy++) {
        for (int cx = 0; cx < 2; cx++) {
            int ox = cx ? w - r : 0;          // 这个角的外接方块
            int oy = cy ? h - r : 0;
            float fx = cx ? (float)(w - r) : (float)r;   // 圆心
            float fy = cy ? (float)(h - r) : (float)r;
            for (int y = 0; y < r; y++) {
                if (oy + y < 0 || oy + y >= h) continue;
                uint16_t* row = (uint16_t*)(base + (size_t)(oy + y) * stride);
                for (int x = 0; x < r; x++) {
                    if (ox + x < 0 || ox + x >= w) continue;
                    float dx = (ox + x) + 0.5f - fx;
                    float dy = (oy + y) + 0.5f - fy;
                    float d  = sqrtf(dx * dx + dy * dy);
                    if (d <= r - 0.5f) continue;                 // 圆内,保留
                    uint16_t* px = &row[ox + x];
                    if (d >= r + 0.5f) { *px = bg; continue; }   // 圆外,涂背景
                    // 边界一档混合
                    int pr = (*px >> 11) & 31, pg = (*px >> 5) & 63, pb = *px & 31;
                    *px = (uint16_t)((((pr + bg_r) / 2) << 11) |
                                     (((pg + bg_g) / 2) << 5)  |
                                      ((pb + bg_b) / 2));
                }
            }
        }
    }
}

// 在 LVGL 任务里把 JPEG 解成 RGB565(声明见文件上方)
void decode_thumb(int i)
{
    if (i < 0 || i >= MAX_SONGS || !g_thumbs[i].buf) return;

    // 解码就是"把这张图画一遍"再把结果拍下来。off-screen 建一个 image,
    // 摆到屏幕外,snapshot 成 RGB565,然后删掉。
    // 套一个固定 THUMB_PX 见方的盒子再拍 —— 封面尺寸【不受我们控制】:
    // 已缓存的歌可以 /cover?w=&h= 指定,但"热门新歌"走的 /thumb 自带 w=50、
    // 也不保证是正方形。不归一化的话每张图大小不一,整排会参差不齐。
    lv_obj_t* box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, THUMB_PX, THUMB_PX);
    lv_obj_set_pos(box, -4 * THUMB_PX, -4 * THUMB_PX);
    // 圆角【烘进图里】,和选中框的圆角对上。
    // 快照是 RGB565、没有 alpha,所以四个角会是盒子的底色 —— 底色取白,
    // 和页面背景一致,角上就看不出来。
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 0, LV_PART_MAIN);   // 圆角在 round_corners 里做
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 尺寸在请求时就要对了(rewrite_size),这里【不缩放】——
    // lv_image_set_scale 是绕轴心缩的,和居中一起用很容易把内容推出可见区,
    // 实测拍出来一片空白。非正方形的图靠居中 + 裁切处理。
    lv_obj_t* tmp = lv_image_create(box);
    lv_image_set_src(tmp, &g_thumbs[i].dsc);
    lv_obj_update_layout(tmp);
    lv_obj_center(tmp);
    lv_obj_update_layout(box);
    lv_draw_buf_t* rgb = lv_snapshot_take(box, LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(box);

    free(g_thumbs[i].buf);          // JPEG 用完就扔
    g_thumbs[i].buf = nullptr;
    g_thumbs[i].dsc = {};
    if (!rgb) return;
    round_corners(rgb, COVER_R, 0xFFFF);      // 白色 = 页面底色
    g_thumbs[i].rgb = rgb;
    g_thumbs[i].ready = true;
    MusicApp::notify_thumb(i);
}


}  // namespace

TDECK_REGISTER_APP(MusicApp)
