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

#include <cJSON.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <string.h>

// 中文字体。LVGL 内建的 Montserrat 只有 ASCII,中文歌名会渲染成一排方框。
// 这两个是小智 78/xiaozhi-fonts 里的 Noto Sans 子集(常用汉字 + ASCII)。
extern "C" const lv_font_t font_noto_sans_basic_16_4;
extern "C" const lv_font_t font_noto_sans_basic_20_4;

namespace {

const char* TAG = "music";
const lv_font_t* F16 = &font_noto_sans_basic_16_4;
const lv_font_t* F20 = &font_noto_sans_basic_20_4;

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
    uint8_t* in  = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    uint8_t* out = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);

    do {
        if (!in || !out) break;
        if (esp_http_client_open(cli, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(cli);

        esp_audio_simple_dec_cfg_t dcfg = {};
        dcfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        if (esp_audio_simple_dec_open(&dcfg, &dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "解码器打开失败");
            break;
        }

        tdeck_audio_init(SAMPLE_RATE);
        g_state.playing = true;
        g_state.pos_s = 0;

        int64_t samples_out = 0;
        int pending = 0;
        while (!g_stop_req) {
            int n = esp_http_client_read(cli, (char*)in + pending, 4096 - pending);
            if (n <= 0 && pending == 0) break;
            if (n > 0) pending += n;

            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = in;
            raw.len    = pending;
            raw.eos    = (n <= 0);

            esp_audio_simple_dec_out_t o = {};
            o.buffer = out;
            o.len    = 8192;

            while (raw.len > 0 && !g_stop_req) {
                o.needed_size = 0;
                esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &o);
                if (e != ESP_AUDIO_ERR_OK) break;
                if (o.decoded_size > 0) {
                    tdeck_speaker_write((int16_t*)out, o.decoded_size / 2, 1000);
                    samples_out += o.decoded_size / 2;
                    g_state.pos_s = (int)(samples_out / SAMPLE_RATE);
                }
                if (raw.consumed == 0) break;
                raw.buffer += raw.consumed;
                raw.len    -= raw.consumed;
            }
            // 没消费完的尾巴挪到开头,下一轮接着凑成完整帧
            if (raw.len > 0 && raw.len < 4096) memmove(in, raw.buffer, raw.len);
            pending = raw.len;
            if (n <= 0) break;
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

void start_play(const char* url, const char* title, const char* artist, int dur)
{
    strncpy(g_play_url, url, sizeof(g_play_url) - 1);
    strncpy(g_state.title,  title  ? title  : "", sizeof(g_state.title) - 1);
    strncpy(g_state.artist, artist ? artist : "", sizeof(g_state.artist) - 1);
    g_state.dur_s = dur;
    g_stop_req = false;
    xTaskCreate(play_task, "music_play", 8192, nullptr, 5, &g_play_task);
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
                   cJSON_IsString(ar) ? ar->valuestring : "", 0);
    }
    cJSON_Delete(root);
    return ok;
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

class MusicApp : public tdeck::App {
public:
    const char* name() const override   { return "Music"; }
    const char* icon() const override   { return LV_SYMBOL_AUDIO; }
    uint32_t    accent() const override { return 0x55853A; }
    // 搜索框里要能打全部字母(ADR-006 的后路)
    bool wants_raw_keys() const override { return searching_; }

    // 卡片是【入口】不是挂件:不显示正在播放什么,只要一眼认得出这是音乐。
    // 一排高低不等的均衡器条 —— 不用读字就知道是什么,而且和其它卡片
    // 在视觉上完全不会混。
    void render_card(lv_obj_t* card) override
    {
        const int W = 143, H = 94;
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1E2A16), LV_PART_MAIN);

        // 高度取一段固定序列而不是随机:每次进桌面看到的是同一张卡片,
        // 随机会让人以为它在动
        static const int hs[] = { 18, 34, 26, 46, 30, 54, 22, 40, 28, 16 };
        const int n = sizeof(hs) / sizeof(hs[0]);
        const int bw = 8, gap = 5;
        int total = n * bw + (n - 1) * gap;
        int x0 = (W - total) / 2;

        for (int i = 0; i < n; i++) {
            lv_obj_t* b = lv_obj_create(card);
            lv_obj_set_size(b, bw, hs[i]);
            lv_obj_set_pos(b, x0 + i * (bw + gap), 20 + (56 - hs[i]) / 2);
            lv_obj_set_style_radius(b, bw / 2, LV_PART_MAIN);
            // 中间几根更亮,做出"中心发光"的层次,不是一排死板的同色条
            uint32_t c = (i >= 3 && i <= 6) ? 0xA8E063 : 0x6E8B4F;
            lv_obj_set_style_bg_color(b, lv_color_hex(c), LV_PART_MAIN);
            lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* nm = lv_label_create(card);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8EFE0), LV_PART_MAIN);
        lv_label_set_text(nm, "Music");
        lv_obj_set_pos(nm, 12, H - 26);
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xF2F5EE), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        mk(root_, F20, C_TEXT, 14, 10, "Music");

        if (g_state.n_songs == 0) {
            loading_ = mk(root_, &lv_font_montserrat_16, C_MUTE, 14, 50, "loading...");
            // 拉歌单是阻塞 HTTP,不能在 LVGL 任务里做,否则整个界面卡住
            xTaskCreate([](void* self) {
                fetch_list();
                lvgl_port_lock(0);
                static_cast<MusicApp*>(self)->build_list();
                lvgl_port_unlock();
                vTaskDelete(nullptr);
            }, "music_list", 8192, this, 4, nullptr);
        } else {
            build_list();
        }
    }

    void on_exit() override { root_ = list_ = search_row_ = nullptr; }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (searching_) return search_input(ev);
        if (g_state.n_songs == 0) {
            if (ev.key == tdeck::Key::Enter) { enter_search(0); return true; }
            return false;
        }
        switch (ev.key) {
        case tdeck::Key::Up:   if (sel_ > -1) { sel_--; paint(); } return true;
        case tdeck::Key::Down: if (sel_ < g_state.n_songs - 1) { sel_++; paint(); } return true;
        case tdeck::Key::Enter:
            // sel_ == -1 是列表顶上的搜索行。
            // 原来做的是"打字即搜索",但 s 会弹快捷键浮层、n 会退出应用
            // (ADR-006 的系统保留键),常见歌名根本起不了头。
            // 显式入口不依赖任何字母,反而更可靠。
            if (sel_ < 0) { enter_search(0); return true; }
            if (g_state.playing) { stop_playback(); return true; }
            {
                char u[400];
                snprintf(u, sizeof(u), MUSIC_BASE_URL "/audio?f=%s.opus&key=" MUSIC_TOKEN,
                         g_songs[sel_].key);
                start_play(u, g_songs[sel_].title, g_songs[sel_].artist, g_songs[sel_].dur);
            }
            return true;

        default: return false;
        }
    }

private:
    void enter_search(char first)
    {
        searching_ = true;
        q_len_ = 0; q_[0] = 0;
        if (first) { q_[q_len_++] = first; q_[q_len_] = 0; }
        lv_obj_clean(root_);
        mk(root_, F20, C_TEXT, 14, 10, "Search");

        lv_obj_t* box = lv_obj_create(root_);
        lv_obj_set_size(box, SCR_W - 24, 46);
        lv_obj_set_pos(box, 12, 44);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        q_lbl_ = mk(box, F20, C_TEXT, 12, 11, q_);

        sr_ = mk(root_, &lv_font_montserrat_14, C_MUTE, 14, 104,
                 "type a song name, Enter to play");
        mk(root_, &lv_font_montserrat_14, C_MUTE, 14, SCR_H - 24,
           "Enter: play      ESC: back");
    }

    bool search_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { searching_ = false; lv_obj_clean(root_);
            mk(root_, F20, C_TEXT, 14, 10, "Music"); build_list(); return true; }
        if (ev.key == tdeck::Key::Enter) {
            if (!q_len_) return true;
            lv_label_set_text(sr_, "searching...");
            lv_refr_now(nullptr);
            // 搜索是阻塞 HTTP(服务端可能要现去下载),丢到任务里做,
            // 否则 LVGL 卡住几十秒,看起来像死机
            static char qcopy[128];
            strncpy(qcopy, q_, sizeof(qcopy) - 1);
            self_ = this;
            xTaskCreate([](void*) {
                bool ok = search_and_play(qcopy);
                lvgl_port_lock(0);
                if (self_ && self_->sr_)
                    lv_label_set_text(self_->sr_, ok ? "playing" : "not found");
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

    void build_list()
    {
        if (!root_) return;
        if (loading_) { lv_obj_delete(loading_); loading_ = nullptr; }
        if (g_state.n_songs == 0) {
            mk(root_, &lv_font_montserrat_16, C_MUTE, 14, 50, "no tracks (check network)");
            return;
        }
        // 搜索行固定在列表上方,不随列表滚动 —— 它是个功能入口不是一条数据
        search_row_ = lv_obj_create(root_);
        lv_obj_set_size(search_row_, SCR_W - 24, 32);
        lv_obj_set_pos(search_row_, 12, 36);
        lv_obj_set_style_bg_color(search_row_, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_radius(search_row_, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(search_row_, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(search_row_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(search_row_, LV_OBJ_FLAG_SCROLLABLE);
        mk(search_row_, F16, C_MUTE, 10, 6, LV_SYMBOL_PLUS "  Search a song");

        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W - 24, 152);
        lv_obj_set_pos(list_, 12, 74);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < g_state.n_songs; i++) {
            lv_obj_t* r = lv_obj_create(list_);
            lv_obj_set_size(r, SCR_W - 28, 46);
            lv_obj_set_pos(r, 0, i * 50);
            lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), LV_PART_MAIN);
            lv_obj_set_style_radius(r, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* t = mk(r, F16, C_TEXT, 10, 3, g_songs[i].title);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, 230);
            lv_obj_t* a = mk(r, F16, C_MUTE, 10, 22, g_songs[i].artist);
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_set_width(a, 230);

            char d[16];
            snprintf(d, sizeof(d), "%d:%02d", g_songs[i].dur / 60, g_songs[i].dur % 60);
            mk(r, &lv_font_montserrat_14, C_MUTE, SCR_W - 70, 15, d);
            rows_[i] = r;
        }
        paint();
    }

    void paint()
    {
        if (search_row_) {
            bool on = (sel_ < 0);
            lv_obj_set_style_border_color(search_row_, lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(search_row_, on ? 2 : 1, LV_PART_MAIN);
        }
        for (int i = 0; i < g_state.n_songs; i++) {
            bool on = (i == sel_);
            lv_obj_set_style_border_color(rows_[i], lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(rows_[i], on ? 2 : 1, LV_PART_MAIN);
        }
        if (sel_ >= 0 && g_state.n_songs) lv_obj_scroll_to_view(rows_[sel_], LV_ANIM_ON);
    }

    lv_obj_t* root_ = nullptr, *list_ = nullptr, *loading_ = nullptr;
    lv_obj_t* rows_[MAX_SONGS] = {};
    lv_obj_t* q_lbl_ = nullptr, *sr_ = nullptr, *search_row_ = nullptr;
    char      q_[128] = {};
    int       q_len_ = 0;
    bool      searching_ = false;
    int       sel_ = 0;
    static MusicApp* self_;
};

MusicApp* MusicApp::self_ = nullptr;

}  // namespace

TDECK_REGISTER_APP(MusicApp)
