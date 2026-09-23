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

namespace {

const char* TAG = "music";

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
    char* body = http_get(url, 32768);
    if (!body) { ESP_LOGW(TAG, "歌单请求失败"); return; }

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
void play_task(void* arg)
{
    int idx = (int)(intptr_t)arg;
    Song& s = g_songs[idx];

    char url[320];
    snprintf(url, sizeof(url), MUSIC_BASE_URL "/audio?f=%s.opus&key=" MUSIC_TOKEN, s.key);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
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
        strncpy(g_state.title,  s.title,  sizeof(g_state.title) - 1);
        strncpy(g_state.artist, s.artist, sizeof(g_state.artist) - 1);
        g_state.dur_s = s.dur;
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
    ESP_LOGI(TAG, "播放结束:%s", s.title);
    vTaskDelete(nullptr);
}

void stop_playback()
{
    if (!g_play_task) return;
    g_stop_req = true;
    for (int i = 0; i < 50 && g_play_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    g_stop_req = false;
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

    // 卡片显示【正在放什么】和进度 —— 这是音乐应用唯一值得占一张卡片的信息。
    // 没在放的时候显示曲库规模,也比一个音符图标有内容。
    void render_card(lv_obj_t* card) override
    {
        if (g_state.playing) {
            mk(card, &lv_font_montserrat_14, C_ACCENT, 12, 10, LV_SYMBOL_PLAY "  Now playing");

            lv_obj_t* t = mk(card, &lv_font_montserrat_16, C_TEXT, 12, 30, g_state.title);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, 119);

            lv_obj_t* a = mk(card, &lv_font_montserrat_14, C_MUTE, 12, 50, g_state.artist);
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_set_width(a, 119);

            // 进度条:比写 "1:23 / 4:05" 直观,而且在这个尺寸上不占地方
            lv_obj_t* track = lv_obj_create(card);
            lv_obj_set_size(track, 119, 4);
            lv_obj_set_pos(track, 12, 76);
            lv_obj_set_style_bg_color(track, lv_color_hex(C_LINE), LV_PART_MAIN);
            lv_obj_set_style_radius(track, 2, LV_PART_MAIN);
            lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
            lv_obj_set_scrollable(track, false);

            int w = g_state.dur_s > 0 ? 119 * g_state.pos_s / g_state.dur_s : 0;
            if (w > 119) w = 119;
            if (w > 0) {
                lv_obj_t* fill = lv_obj_create(track);
                lv_obj_set_size(fill, w, 4);
                lv_obj_set_pos(fill, 0, 0);
                lv_obj_set_style_bg_color(fill, lv_color_hex(C_ACCENT), LV_PART_MAIN);
                lv_obj_set_style_radius(fill, 2, LV_PART_MAIN);
                lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
                lv_obj_set_style_pad_all(fill, 0, LV_PART_MAIN);
            }
        } else {
            mk(card, &lv_font_montserrat_14, C_MUTE, 12, 12, LV_SYMBOL_AUDIO "  Music");
            char b[48];
            snprintf(b, sizeof(b), "%d", g_state.n_songs);
            mk(card, &lv_font_montserrat_36, C_TEXT, 12, 28, b);
            mk(card, &lv_font_montserrat_14, C_MUTE, 12, 70, "recent tracks");
        }
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xF2F5EE), LV_PART_MAIN);
        lv_obj_set_scrollable(root, false);
        mk(root_, &lv_font_montserrat_20, C_TEXT, 14, 10, "Music");

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

    void on_exit() override { root_ = nullptr; list_ = nullptr; }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (g_state.n_songs == 0) return false;
        switch (ev.key) {
        case tdeck::Key::Up:   if (sel_ > 0) { sel_--; paint(); } return true;
        case tdeck::Key::Down: if (sel_ < g_state.n_songs - 1) { sel_++; paint(); } return true;
        case tdeck::Key::Enter:
            if (g_state.playing) stop_playback();
            else {
                g_stop_req = false;
                xTaskCreate(play_task, "music_play", 8192,
                            (void*)(intptr_t)sel_, 5, &g_play_task);
            }
            return true;
        default: return false;
        }
    }

private:
    void build_list()
    {
        if (!root_) return;
        if (loading_) { lv_obj_delete(loading_); loading_ = nullptr; }
        if (g_state.n_songs == 0) {
            mk(root_, &lv_font_montserrat_16, C_MUTE, 14, 50, "no tracks (check network)");
            return;
        }
        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W - 24, 182);
        lv_obj_set_pos(list_, 12, 38);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < g_state.n_songs; i++) {
            lv_obj_t* r = lv_obj_create(list_);
            lv_obj_set_size(r, SCR_W - 28, 40);
            lv_obj_set_pos(r, 0, i * 44);
            lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), LV_PART_MAIN);
            lv_obj_set_style_radius(r, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
            lv_obj_set_scrollable(r, false);

            lv_obj_t* t = mk(r, &lv_font_montserrat_16, C_TEXT, 10, 3, g_songs[i].title);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, 230);
            lv_obj_t* a = mk(r, &lv_font_montserrat_14, C_MUTE, 10, 21, g_songs[i].artist);
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_set_width(a, 230);

            char d[16];
            snprintf(d, sizeof(d), "%d:%02d", g_songs[i].dur / 60, g_songs[i].dur % 60);
            mk(r, &lv_font_montserrat_14, C_MUTE, SCR_W - 70, 12, d);
            rows_[i] = r;
        }
        paint();
    }

    void paint()
    {
        for (int i = 0; i < g_state.n_songs; i++) {
            bool on = (i == sel_);
            lv_obj_set_style_border_color(rows_[i], lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(rows_[i], on ? 2 : 1, LV_PART_MAIN);
        }
        if (g_state.n_songs) lv_obj_scroll_to_view(rows_[sel_], LV_ANIM_ON);
    }

    lv_obj_t* root_ = nullptr, *list_ = nullptr, *loading_ = nullptr;
    lv_obj_t* rows_[MAX_SONGS] = {};
    int sel_ = 0;
};

}  // namespace

TDECK_REGISTER_APP(MusicApp)
