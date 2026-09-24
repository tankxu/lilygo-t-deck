// music_app.cc — 音乐播放器
//
// 后端是 nuc 上自建的 xiaozhi-music-mcp。它把所有音频统一转成
// 24kHz 单声道 Ogg/Opus(约 4~8 KB/s),正是为 ESP32 这点算力和带宽设计的 ——
// 固件不用重采样、不用切采样率,拉下来解码直接送 I2S。
//
// 列表走 /api/reco?kind=mine(最近播放),音频走 /audio?f=<key>.opus。
// 两个接口都用 ?key=<token> 鉴权,token 在 secrets.h 里(不进版本库)。

#include "app.h"
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

const char* TAG = "music";
// 中文字体没加载成功就回落到 Montserrat —— 宁可显示方框,不要空指针崩溃
inline const lv_font_t* F16() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_16; }
inline const lv_font_t* F20() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_20; }



constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_TEXT   = 0x1b2117;
constexpr uint32_t C_MUTE   = 0x6a7360;
constexpr uint32_t C_LINE   = 0xd6dbcd;
constexpr uint32_t C_CARD   = 0xffffff;
constexpr int SCR_W = 320, SCR_H = 240;

constexpr int MAX_SONGS   = 24;
constexpr int SAMPLE_RATE = 24000;

struct Song {
    char key[40];
    char title[64];
    char artist[48];
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

    int n = 0;
    // 服务端把最近播放放在 recent 里
    cJSON* arr = cJSON_GetObjectItem(root, "recent");
    if (cJSON_IsArray(arr)) {
        cJSON* it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= MAX_SONGS) break;
            cJSON* k = cJSON_GetObjectItem(it, "key");
            cJSON* t = cJSON_GetObjectItem(it, "title");
            cJSON* a = cJSON_GetObjectItem(it, "artist");
            cJSON* d = cJSON_GetObjectItem(it, "dur");
            if (!cJSON_IsString(k) || !cJSON_IsString(t)) continue;
            strncpy(g_songs[n].key,   k->valuestring, sizeof(g_songs[n].key) - 1);
            strncpy(g_songs[n].title, t->valuestring, sizeof(g_songs[n].title) - 1);
            strncpy(g_songs[n].artist,
                    cJSON_IsString(a) ? a->valuestring : "", sizeof(g_songs[n].artist) - 1);
            g_songs[n].dur = cJSON_IsNumber(d) ? d->valueint : 0;
            n++;
        }
    }
    cJSON_Delete(root);
    g_state.n_songs = n;
    ESP_LOGI(TAG, "歌单 %d 首", n);
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

bool fetch_cover(const char* key)
{
    char url[400];
    snprintf(url, sizeof(url),
             MUSIC_BASE_URL "/cover?f=%s.jpg&w=120&h=120&key=" MUSIC_TOKEN, key);

    esp_http_client_config_t cfg = {};
    cfg.url = url; cfg.timeout_ms = 8000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    constexpr int CAP = 48 * 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(cli); return false; }

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
    if (len < 100) { free(buf); return false; }

    if (g_cover_buf) free(g_cover_buf);
    g_cover_buf = buf;
    g_cover_dsc = {};
    g_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_cover_dsc.header.cf    = LV_COLOR_FORMAT_RAW;   // 让解码器链去认格式
    g_cover_dsc.header.w     = 120;
    g_cover_dsc.header.h     = 120;
    g_cover_dsc.data         = g_cover_buf;
    g_cover_dsc.data_size    = len;
    ESP_LOGI(TAG, "封面 %d 字节", len);
    return true;
}

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
enum class View { Library, NowPlaying, Search };

class MusicApp : public tdeck::App {
public:
    const char* name() const override   { return "Music"; }
    const char* icon() const override   { return LV_SYMBOL_AUDIO; }
    uint32_t    accent() const override { return 0x55853A; }
    bool wants_raw_keys() const override { return view_ == View::Search; }

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
        case View::NowPlaying: return now_input(ev);
        case View::Library:    return lib_input(ev);
        }
        return false;
    }

private:
    void clear_refs()
    {
        list_ = search_row_ = q_lbl_ = sr_ = nullptr;
        cover_ = title_ = artist_ = bar_fill_ = t_cur_ = t_tot_ = btn_play_ = nullptr;

        for (auto& r : rows_) r = nullptr;
        for (auto& m : play_mark_) m = nullptr;
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
        lv_obj_set_height(title_, 52);

        artist_ = lbl(root_, F16(), 0x6A7360, RX, 104, g_state.artist);
        lv_label_set_long_mode(artist_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(artist_, RW);
        lv_obj_set_height(artist_, 22);

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
        mk_pill(12, 10, LV_SYMBOL_LEFT, "Back", [](lv_event_t* e) {
            (void)e; tdeck::launcher_back();
        });
        mk_pill(88, 10, LV_SYMBOL_LIST, "Playlist", [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->show_library();
        });

        // ── 底部控制条 ────────────────────────────────────
        // 封面占到 y=164,所有按钮必须整体落在它【下面】。之前按钮 cy 是
        // 178/182、直径 56,上沿到 150,被封面压住了一半 —— 一行统一 cy=196,
        // 最大的那颗上沿也只到 168,不会再撞。
        //
        // 音量胶囊靠左,传输键组在【剩下的空间里】居中(不是全屏居中):
        // 胶囊右沿 76,屏宽 320,所以组心在 198,左右各留 48。全屏居中的话
        // 右边会空出 86px,看着像掉了一个按钮。
        constexpr int ROW_Y = 196;
        mk_vol_pill(42, ROW_Y);
        mk_btn(146, ROW_Y, 44, LV_SYMBOL_PREV, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->skip(-1);
        });
        btn_play_ = mk_btn(198, ROW_Y, 56, LV_SYMBOL_PAUSE, [](lv_event_t* e) {
            static_cast<MusicApp*>(lv_event_get_user_data(e))->toggle();
        });
        mk_btn(250, ROW_Y, 44, LV_SYMBOL_NEXT, [](lv_event_t* e) {
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

        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
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

    // ── 曲库 ──────────────────────────────────────────────
    void show_library()
    {
        view_ = View::Library;
        lv_obj_clean(root_); clear_refs();
        lbl(root_, &lv_font_montserrat_14, 0x8A9480, 16, 14, "LIBRARY");

        if (g_state.playing) {
            // 顶部一条"正在播放",点了回到播放页 —— 手机音乐 app 的标配
            lv_obj_t* mini = lv_obj_create(root_);
            lv_obj_set_size(mini, SCR_W - 28, 30);
            lv_obj_set_pos(mini, 14, 34);
            lv_obj_set_style_bg_color(mini, lv_color_hex(0xE8EEE0), LV_PART_MAIN);
            lv_obj_set_style_radius(mini, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(mini, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(mini, 0, LV_PART_MAIN);
            lv_obj_remove_flag(mini, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(mini, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(mini, [](lv_event_t* e) {
                static_cast<MusicApp*>(lv_event_get_user_data(e))->show_now();
            }, LV_EVENT_CLICKED, this);
            // 定宽【还要】定高:LONG_DOT 是按高度截的,只限宽的话长标题会
            // 换到第二行,从这条 30px 高的小条里溢出去,盖住下面的搜索框。
            lv_obj_t* l = lbl(mini, F16(), 0x55853A, 10, 6, g_state.title);
            lv_obj_set_size(l, 232, 18);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lbl(mini, &lv_font_montserrat_16, 0x55853A, SCR_W - 60, 5, LV_SYMBOL_PLAY);
        }

        int top = g_state.playing ? 70 : 34;
        search_row_ = lv_obj_create(root_);
        lv_obj_set_size(search_row_, SCR_W - 28, 30);
        lv_obj_set_pos(search_row_, 14, top);
        lv_obj_set_style_bg_color(search_row_, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
        lv_obj_set_style_radius(search_row_, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(search_row_, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(search_row_, lv_color_hex(0xD2DAC4), LV_PART_MAIN);
        lv_obj_set_style_pad_all(search_row_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(search_row_, LV_OBJ_FLAG_SCROLLABLE);
        lbl(search_row_, F16(), 0x6A7360, 10, 5, LV_SYMBOL_PLUS "  Search a song");

        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W - 28, SCR_H - top - 44);
        lv_obj_set_pos(list_, 14, top + 38);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < g_state.n_songs; i++) {
            lv_obj_t* r = lv_obj_create(list_);
            lv_obj_set_size(r, SCR_W - 32, 42);
            lv_obj_set_pos(r, 0, i * 46);
            lv_obj_set_style_bg_color(r, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
            lv_obj_set_style_radius(r, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_user_data(r, (void*)(intptr_t)i);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(r, [](lv_event_t* e) {
                auto* s = static_cast<MusicApp*>(lv_event_get_user_data(e));
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                s->stop_and_play(idx);
            }, LV_EVENT_CLICKED, this);

            // LV_LABEL_LONG_DOT 是按【高度】截的:只设宽度,长标题会换到第二行,
            // 正好盖住下面固定 y 的歌手名。必须把高度也钉成一行。
            lv_obj_t* tl = lbl(r, F16(), 0x1B2117, 10, 3, g_songs[i].title);
            lv_obj_set_size(tl, 210, 18);
            lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
            lv_obj_t* al = lbl(r, F16(), 0x8A9480, 10, 22, g_songs[i].artist);
            lv_obj_set_size(al, 210, 18);
            lv_label_set_long_mode(al, LV_LABEL_LONG_DOT);
            char d[16];
            snprintf(d, sizeof(d), "%d:%02d", g_songs[i].dur / 60, g_songs[i].dur % 60);
            lbl(r, &lv_font_montserrat_14, 0x8A9480, SCR_W - 66, 12, d);
            play_mark_[i] = lbl(r, &lv_font_montserrat_16, 0x55853A, SCR_W - 92, 11, LV_SYMBOL_PLAY);
            lv_obj_add_flag(play_mark_[i], LV_OBJ_FLAG_HIDDEN);
            rows_[i] = r;
        }
        paint(); paint_playing();
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
            lv_obj_set_style_border_color(search_row_,
                lv_color_hex(on ? 0x55853A : 0xD2DAC4), LV_PART_MAIN);
            lv_obj_set_style_border_width(search_row_, on ? 2 : 1, LV_PART_MAIN);
        }
        for (int i = 0; i < g_state.n_songs; i++) {
            if (!rows_[i]) continue;
            bool on = (i == sel_);
            lv_obj_set_style_border_color(rows_[i],
                lv_color_hex(on ? 0x55853A : 0xE0E5D8), LV_PART_MAIN);
            lv_obj_set_style_border_width(rows_[i], on ? 2 : 1, LV_PART_MAIN);
        }
        if (sel_ >= 0 && g_state.n_songs && rows_[sel_])
            lv_obj_scroll_to_view(rows_[sel_], LV_ANIM_ON);
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

    bool lib_input(const tdeck::InputEvent& ev)
    {
        if (g_state.n_songs == 0) {
            if (ev.key == tdeck::Key::Enter) { enter_search(0); return true; }
            return false;
        }
        switch (ev.key) {
        case tdeck::Key::Up:   if (sel_ > -1) { sel_--; paint(); } return true;
        case tdeck::Key::Down: if (sel_ < g_state.n_songs - 1) { sel_++; paint(); } return true;
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

        sr_ = lbl(root_, F16(), 0x8A9480, 16, 104, "type a song name, Enter to play");
        lbl(root_, &lv_font_montserrat_14, 0x9AA490, 16, SCR_H - 24, "Enter: play      ESC: back");
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
    lv_obj_t* play_mark_[MAX_SONGS] = {};
    lv_timer_t* tick_ = nullptr;
    char q_[128] = {};
    int  q_len_ = 0, sel_ = 0, playing_idx_ = -1;
    View view_ = View::Library;
    static MusicApp* self_;
};

MusicApp* MusicApp::self_ = nullptr;

}  // namespace

TDECK_REGISTER_APP(MusicApp)
