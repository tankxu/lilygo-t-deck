#include "learning.h"
#include "formula.h"
#include "gif_player.h"
#include "ui/fonts.h"
#include "ui/jpeg_size.h"
#include "secrets.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// 素材服务。secrets.h 里可以覆盖(自建服务时),不填就用默认的 R2 自有域名。
#ifndef LEARN_BASE_URL
#define LEARN_BASE_URL "https://pm-learn.tankxu.com/learn"
#endif

// 公式正文用的 28px 数学字体(main/assets/fonts/PuHuiMath28.c)。
// ⚠️ LV_FONT_DECLARE 必须在匿名 namespace【外面】—— 放进去就成了匿名符号,链接不到。
LV_FONT_DECLARE(PuHuiMath28);
LV_FONT_DECLARE(IpaSans20);
LV_FONT_DECLARE(MontserratSemiBold26);

namespace tdeck {
namespace learning {
namespace {

const char* TAG = "learning";

// 倒计时:画面出来后顶部那条走完就自动收。
// 没这条的话页面会一直盖着,孩子看完了还挡着。
constexpr uint32_t CARD_SEC      = 20;
// ⚠️ 笔顺不能用固定倒计时。
// GIF 标称 7.1 秒,实测在板子上要 18 秒(解码 + 重绘跟不上);而且不同字差别很大 ——
// 8 画的「学」41 帧,15 画的字有 80 多帧,固定 30 秒对后者必然半路消失。
// 所以:播放期间只挂一个【上限】兜底,等 gif_player 报 LV_EVENT_READY(播完)
// 之后再走一小段倒计时,让写完的字停一会儿。
constexpr uint32_t STROKE_CAP_SEC   = 150;   // 兜底上限,正常走不到
constexpr uint32_t STROKE_LINGER_SEC = 8;    // 播完之后停多久
constexpr uint32_t PAGE_BASE_SEC = 20;
constexpr uint32_t PAGE_MAX_SEC  = 90;
constexpr uint32_t IDLE_MS       = 60 * 1000;    // 一直没出画面(卡在下载)的兜底
constexpr size_t   MAX_BYTES     = 384 * 1024;

constexpr uint32_t C_GREEN = 0x86E23C;           // 标题与倒计时条

enum class Want { None, Media, Fallback, Page, Formula };

SemaphoreHandle_t s_mu = nullptr;

// ── 状态(改之前拿 s_mu)────────────────────────────────────
uint32_t s_gen = 0;              // 请求代号:下载完发现变了就丢弃
Want     s_want = Want::None;
bool     s_visible = false;      // 想不想显示
bool     s_dirty = false;        // 状态变了,定时器要动手
int64_t  s_last_action_us = 0;
int64_t  s_deadline_us = 0;      // 画面出来后才设;0 = 还没出画面

// 下载回来的原始字节。JPEG 和 GIF 都是原样交给 LVGL ——
// JPEG 走 LV_COLOR_FORMAT_RAW 让解码器链自己认(音乐封面同一条路),
// GIF 交给 lv_gif。所以这里不需要 jpeg_to_image 那种先解成位图的步骤。
uint8_t* s_bytes = nullptr;
size_t   s_bytes_len = 0;
bool     s_is_gif = false;
char     s_label[64] = {};

char s_text[160] = {}, s_guide[160] = {};        // 简卡兜底用
char s_meaning[96] = {};                         // 英文词的中文释义(设备自己画)
char s_title[96] = {}, s_body[1024] = {};        // 文字页
char s_f_title[96] = {}, s_f_src[512] = {}, s_f_note[192] = {};

// ── LVGL 对象(只在定时器里碰)──────────────────────────────
lv_obj_t* s_root = nullptr;
lv_obj_t* s_bar  = nullptr;
lv_obj_t* s_content = nullptr;
lv_image_dsc_t s_dsc{};
uint8_t* s_shown_bytes = nullptr;   // 正在显示的那份,解码器直接引用着,不能提前释放

int64_t now_us() { return esp_timer_get_time(); }
const lv_font_t* cjk() { const lv_font_t* f = font_cjk(); return f ? f : &lv_font_montserrat_20; }

void lock()   { xSemaphoreTake(s_mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mu); }

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

size_t utf8_chars(const char* s)
{
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}

// ── 下载 ──────────────────────────────────────────────────
struct Job { char url[320]; char label[64]; bool gif; uint32_t gen; };

void fetch_task(void* arg)
{
    Job* job = (Job*)arg;

    esp_http_client_config_t cfg = {};
    cfg.url = job->url;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;      // R2 是 HTTPS
    esp_http_client_handle_t h = esp_http_client_init(&cfg);

    uint8_t* buf = nullptr;
    int len = 0, status = 0;

    if (h && esp_http_client_open(h, 0) == ESP_OK) {
        const int64_t total = esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (status == 200 && total > 0 && total <= (int64_t)MAX_BYTES) {
            // 图放 PSRAM:320x240 的卡 8~12KB,笔顺 GIF 十几 KB,
            // 但内部 RAM 很紧(小智起来之后只剩几十 KB),没必要占它
            buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf) {
                while (len < (int)total) {
                    int r = esp_http_client_read(h, (char*)buf + len, (int)total - len);
                    if (r <= 0) break;
                    len += r;
                }
                if (len != (int)total) { heap_caps_free(buf); buf = nullptr; len = 0; }
            }
        }
        esp_http_client_close(h);
    }
    if (h) esp_http_client_cleanup(h);

    lock();
    if (job->gen != s_gen) {
        // 用户在下载期间又要了别的 —— 这份已经没人要了
        ESP_LOGI(TAG, "丢弃过期的「%s」", job->label);
        if (buf) heap_caps_free(buf);
    } else if (buf) {
        if (s_bytes) heap_caps_free(s_bytes);
        s_bytes = buf; s_bytes_len = (size_t)len;
        // 服务端说是什么不算数,看字节头 —— 万一 .gif 回了 JPEG 也能显示
        s_is_gif = len > 6 && (memcmp(buf, "GIF87a", 6) == 0 || memcmp(buf, "GIF89a", 6) == 0);
        strncpy(s_label, job->label, sizeof(s_label) - 1);
        s_want = Want::Media; s_visible = true; s_dirty = true;
        s_last_action_us = now_us();
        ESP_LOGI(TAG, "「%s」到手 %d 字节(%s)", job->label, len, s_is_gif ? "GIF" : "JPEG");
    } else {
        ESP_LOGW(TAG, "「%s」取不到(HTTP %d)", job->label, status);
        // 404 = 这个词没预渲染(生僻词/不在词表里)。设备自己画一张简卡,
        // 总比什么都不显示强 —— 模型已经在嘴上讲了,屏幕不该是空的。
        if (!job->gif && status == 404) {
            s_want = Want::Fallback; s_visible = true; s_dirty = true;
            s_last_action_us = now_us();
        }
    }
    unlock();

    delete job;
    vTaskDelete(nullptr);
}

void fetch_async(const char* url, const char* label, bool gif)
{
    Job* job = new Job{};
    strncpy(job->url, url, sizeof(job->url) - 1);
    strncpy(job->label, label, sizeof(job->label) - 1);
    job->gif = gif;
    lock(); job->gen = ++s_gen; s_last_action_us = now_us(); s_deadline_us = 0; unlock();
    // 8KB:纯 HTTP + memcpy,但 TLS 握手在这个栈上,不能再小
    if (xTaskCreate(fetch_task, "learn_dl", 8192, job, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "建下载任务失败");
        delete job;
    }
}

// ── 画面 ──────────────────────────────────────────────────
void start_countdown(uint32_t sec);

void start_countdown(uint32_t sec)
{
    lock(); s_deadline_us = now_us() + (int64_t)sec * 1000000; unlock();
    if (!s_bar) return;
    lv_anim_delete(s_bar, nullptr);
    lv_bar_set_value(s_bar, 1000, LV_ANIM_OFF);
    lv_obj_move_foreground(s_bar);      // 内容是后建的,得把条再提上来
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bar);
    lv_anim_set_values(&a, 1000, 0);
    lv_anim_set_duration(&a, sec * 1000);
    lv_anim_set_exec_cb(&a, [](void* t, int32_t v) { lv_bar_set_value((lv_obj_t*)t, v, LV_ANIM_OFF); });
    lv_anim_start(&a);
}

void build_root()
{
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 320, 240);
    // 服务端的卡是黑底白字,GIF 240x240 居中,所以整层也用黑 —— 四周不留亮边
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, [](lv_event_t*) {
        // 点一下就收。stackchan 那边要敲两下(桌面机器人,小孩会误碰,
        // 而且单击在那儿还有"掐断说话"的含义);T-Deck 是拿在手里的,
        // 这层盖着的时候单击没有别的意思,一下收掉更直接。
        clear();
    }, LV_EVENT_CLICKED, nullptr);

    s_bar = lv_bar_create(s_root);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, 320, 3);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 1000, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);   // 点它也算点整页
}

void drop_content()
{
    if (s_content) lv_obj_delete(s_content);
    s_content = nullptr;
    // ⚠️ 显示中的字节要等控件删掉【之后】才能释放 —— 解码器一直引用着它
    if (s_shown_bytes) { heap_caps_free(s_shown_bytes); s_shown_bytes = nullptr; }
}

void destroy_all()
{
    drop_content();
    if (s_bar) lv_anim_delete(s_bar, nullptr);
    s_bar = nullptr;                 // 跟 s_root 一起删
    if (s_root) lv_obj_delete(s_root);
    s_root = nullptr;
}

lv_obj_t* new_box()
{
    lv_obj_t* box = lv_obj_create(s_root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 320, 240);
    lv_obj_center(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);   // 点它要穿到 s_root 上去
    return box;
}

void show_media()
{
    drop_content();
    lock();
    uint8_t* bytes = s_bytes; size_t len = s_bytes_len; bool gif = s_is_gif;
    s_bytes = nullptr; s_bytes_len = 0;               // 所有权交给显示侧
    char label[64]; strncpy(label, s_label, sizeof(label));
    unlock();
    if (!bytes) return;

    s_shown_bytes = bytes;
    s_dsc = lv_image_dsc_t{};
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.data      = bytes;
    s_dsc.data_size = (uint32_t)len;

    if (gif) {
        // 自己的播放器:只让变化过的那块失效(见 gif_player.h)。
        // 用 lv_gif 的话每帧整幅失效,240x240 的 ARGB8888 画布在 PSRAM 上,
        // 实测一轮 7.1 秒的动画要跑 18 秒,还把音频挤卡。
        s_content = gif_player_create(s_root, bytes, len);
        if (s_content) {
            lv_obj_add_event_cb(s_content, [](lv_event_t*) {
                // 播完了才开始倒计时 —— 在这之前只有上限兜底
                start_countdown(STROKE_LINGER_SEC);
            }, LV_EVENT_READY, nullptr);
        }
    } else {
        // 和音乐封面同一条路:RAW + 真实宽高,交给 LVGL 的解码器链(TJPGD)。
        // 宽高必须从字节里读,见 ui/jpeg_size.h。
        s_dsc.header.cf = LV_COLOR_FORMAT_RAW;
        int w = 320, h = 240;
        jpeg_size(bytes, (int)len, &w, &h);
        s_dsc.header.w = (uint32_t)w;
        s_dsc.header.h = (uint32_t)h;
        s_content = lv_image_create(s_root);
        lv_image_set_src(s_content, &s_dsc);
    }
    if (!s_content) { ESP_LOGE(TAG, "「%s」建不起显示对象", label); return; }
    lv_obj_center(s_content);

    // 英文词卡补一行中文释义。
    //
    // 服务端的卡只排了单词 + IPA + 音节,没有翻译。逐行量过 320x240 的卡:
    //   两行注音时  y 105-174 空白
    //   一行注音时  y 119-191 空白
    // 交集是 y 119-174,所以放在屏幕中心偏下 26px 处,两种排版都不压到东西。
    // 中文卡不画 —— 拼音已经在卡上了,再加一行反而乱。
    if (!gif) {
        char meaning[96];
        lock(); strncpy(meaning, s_meaning, sizeof(meaning)); unlock();
        bool ascii = label[0] != 0;
        for (const unsigned char* q = (const unsigned char*)label; *q; q++)
            if (*q > 127) { ascii = false; break; }
        if (ascii && meaning[0]) {
            lv_obj_t* m = lv_label_create(s_root);
            lv_obj_set_style_text_font(m, cjk(), 0);
            lv_obj_set_style_text_color(m, lv_color_hex(C_GREEN), 0);
            lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
            lv_obj_set_width(m, 300);
            lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(m, meaning);
            lv_obj_align(m, LV_ALIGN_CENTER, 0, 26);
            lv_obj_move_foreground(s_bar);   // 倒计时条要压在最上面
        }
    }
    // GIF:先挂上限,真正的倒计时等播完(LV_EVENT_READY)再开始
    start_countdown(gif ? STROKE_CAP_SEC : CARD_SEC);
    ESP_LOGI(TAG, "显示%s「%s」", gif ? "笔顺" : "卡片", label);
}

// 卡片 404 的兜底:设备自己画。英文用拉丁字体 + IPA,中文用全字库。
void show_fallback()
{
    drop_content();
    lock();
    char text[160], guide[160];
    strncpy(text, s_text, sizeof(text)); strncpy(guide, s_guide, sizeof(guide));
    unlock();

    bool ascii = text[0] != 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) if (*p > 127) { ascii = false; break; }

    lv_obj_t* box = new_box();
    lv_obj_t* word = lv_label_create(box);
    lv_obj_set_style_text_color(word, lv_color_white(), 0);
    lv_obj_set_width(word, 300);
    lv_obj_set_style_text_align(word, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(word, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(word, ascii ? &MontserratSemiBold26 : cjk(), 0);
    lv_label_set_text(word, text);
    lv_obj_align(word, LV_ALIGN_CENTER, 0, ascii ? -30 : -24);

    if (guide[0]) {
        lv_obj_t* g = lv_label_create(box);
        lv_obj_set_style_text_font(g, ascii ? &IpaSans20 : cjk(), 0);
        lv_obj_set_style_text_color(g, lv_color_hex(0xBBBBBB), 0);
        lv_label_set_text(g, guide);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, ascii ? 16 : 14);
    }
    {   // 简卡也补释义 —— 生僻词更需要知道意思
        char meaning[96];
        lock(); strncpy(meaning, s_meaning, sizeof(meaning)); unlock();
        if (ascii && meaning[0]) {
            lv_obj_t* m = lv_label_create(box);
            lv_obj_set_style_text_font(m, cjk(), 0);
            lv_obj_set_style_text_color(m, lv_color_hex(C_GREEN), 0);
            lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
            lv_obj_set_width(m, 300);
            lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(m, meaning);
            lv_obj_align(m, LV_ALIGN_CENTER, 0, 48);
        }
    }
    s_content = box;
    start_countdown(CARD_SEC);
    ESP_LOGI(TAG, "简卡「%s」(%s)", text, ascii ? "拉丁" : "中文");
}

void show_page_ui()
{
    drop_content();
    lock();
    char title[96], body[1024];
    strncpy(title, s_title, sizeof(title)); strncpy(body, s_body, sizeof(body));
    unlock();

    lv_obj_t* box = new_box();
    int top = 12;
    if (title[0]) {
        lv_obj_t* t = lv_label_create(box);
        lv_obj_set_style_text_font(t, cjk(), 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_GREEN), 0);
        lv_obj_set_width(t, 296);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(t, title);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, top);
        top += 34;
    }
    lv_obj_t* scroll = lv_obj_create(box);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_size(scroll, 300, 240 - top - 8);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* b = lv_label_create(scroll);
    lv_obj_set_style_text_font(b, cjk(), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_text_line_space(b, 8, 0);
    lv_obj_set_width(b, 296);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    // 短文本(古诗那种一行一句)居中好看,长段落左对齐
    lv_obj_set_style_text_align(b, strlen(body) < 160 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(b, body);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 0);

    s_content = box;
    uint32_t sec = PAGE_BASE_SEC + (uint32_t)utf8_chars(body) / 2;
    if (sec > PAGE_MAX_SEC) sec = PAGE_MAX_SEC;
    start_countdown(sec);
    ESP_LOGI(TAG, "文字页「%s」%u 秒", title, (unsigned)sec);
}

void show_formula_ui()
{
    drop_content();
    lock();
    char title[96], src[512], note[192];
    strncpy(title, s_f_title, sizeof(title));
    strncpy(src, s_f_src, sizeof(src));
    strncpy(note, s_f_note, sizeof(note));
    unlock();

    lv_obj_t* box = new_box();
    int top = 10, bottom = 232;
    if (title[0]) {
        lv_obj_t* t = lv_label_create(box);
        lv_obj_set_style_text_font(t, cjk(), 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_GREEN), 0);
        lv_obj_set_width(t, 296);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(t, title);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, top);
        top += 32;
    }
    lv_obj_t* n = nullptr;
    if (note[0]) {
        lv_point_t sz;
        lv_text_get_size(&sz, note, cjk(), 0, 4, 300, LV_TEXT_FLAG_NONE);
        n = lv_label_create(box);
        lv_obj_set_style_text_font(n, cjk(), 0);
        lv_obj_set_style_text_color(n, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_line_space(n, 4, 0);
        lv_obj_set_width(n, 300);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(n, note);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -6);
        bottom -= sz.y + 8;
    }

    lv_obj_t* f = formula_render(box, src, &PuHuiMath28, cjk());
    lv_obj_update_layout(f);
    int fw = lv_obj_get_width(f), fh = lv_obj_get_height(f);
    if (fh > bottom - top && n) {     // 太高放不下:先牺牲底部说明,公式优先
        lv_obj_delete(n); n = nullptr; bottom = 232;
    }
    if (fh > bottom - top) {          // 还是放不下:整体退回 20px 紧凑版重排
        lv_obj_delete(f);
        f = formula_render(box, src, cjk(), cjk());
        lv_obj_update_layout(f);
        fw = lv_obj_get_width(f); fh = lv_obj_get_height(f);
    }
    int x = (320 - fw) / 2; if (x < 0) x = 0;
    int y = top + (bottom - top - fh) / 2; if (y < top) y = top;
    lv_obj_set_pos(f, x, y);

    s_content = box;
    size_t lines = 1;
    for (size_t k = 0; k + 1 < strlen(src); k++) if (src[k] == '\\' && src[k + 1] == '\\') lines++;
    uint32_t sec = 30 + 8 * (uint32_t)(lines - 1);
    if (sec > PAGE_MAX_SEC) sec = PAGE_MAX_SEC;
    start_countdown(sec);
    ESP_LOGI(TAG, "公式「%s」%dx%d %u 行", title, fw, fh, (unsigned)lines);
}

// LVGL 定时器:跑在 LVGL 任务里,锁是持着的,可以放心建控件
void tick(lv_timer_t*)
{
    lock();
    const int64_t now = now_us();
    if (s_visible) {
        const bool expired = s_deadline_us ? (now >= s_deadline_us)
                                           : (now - s_last_action_us > (int64_t)IDLE_MS * 1000);
        if (expired) {
            s_visible = false; s_dirty = true;
            ESP_LOGI(TAG, "%s,收起", s_deadline_us ? "倒计时走完" : "一直没出画面");
        }
    }
    const bool visible_now = s_visible;
    const bool dirty = s_dirty;
    const Want want = s_want;
    s_dirty = false;
    if (!dirty) { unlock(); return; }
    if (visible_now) s_want = Want::None;
    unlock();

    if (!visible_now) {
        if (s_root) { destroy_all(); ESP_LOGI(TAG, "已隐藏"); }
        lock();
        if (s_bytes) { heap_caps_free(s_bytes); s_bytes = nullptr; s_bytes_len = 0; }
        s_deadline_us = 0;
        unlock();
        return;
    }

    if (!s_root) build_root();
    switch (want) {
    case Want::Media:    show_media();      break;
    case Want::Fallback: show_fallback();   break;
    case Want::Page:     show_page_ui();    break;
    case Want::Formula:  show_formula_ui(); break;
    default: break;
    }
}

// 文字页/公式页共用:作废在途下载,置好状态让定时器接手
void arm(Want w)
{
    s_gen++;                                  // 正在下载的卡片作废,本地内容优先
    if (s_bytes) { heap_caps_free(s_bytes); s_bytes = nullptr; s_bytes_len = 0; }
    s_want = w;
    s_visible = true;
    s_dirty = true;
    s_last_action_us = now_us();
    s_deadline_us = 0;
}

}  // namespace

void begin()
{
    if (s_mu) return;
    s_mu = xSemaphoreCreateMutex();
    // 200ms 够了:倒计时条是 LVGL 动画自己在走,这个定时器只负责
    // "状态变了要建/拆控件"和"到点收起"
    lv_timer_create(tick, 200, nullptr);
    ESP_LOGI(TAG, "学习卡片就绪(素材服务 %s)", LEARN_BASE_URL);
}

void show_card(const char* text, const char* guide, const char* meaning)
{
    if (!s_mu || !text || !text[0]) return;
    char enc[256], url[320];
    urlencode(text, enc, sizeof(enc));
    snprintf(url, sizeof(url), "%s/card/%s.jpg", LEARN_BASE_URL, enc);
    lock();
    strncpy(s_text, text, sizeof(s_text) - 1);
    strncpy(s_guide, guide ? guide : "", sizeof(s_guide) - 1);
    strncpy(s_meaning, meaning ? meaning : "", sizeof(s_meaning) - 1);
    unlock();
    fetch_async(url, text, false);
}

void show_stroke_order(const char* character)
{
    if (!s_mu || !character || !character[0]) return;
    char enc[256], url[320];
    urlencode(character, enc, sizeof(enc));
    snprintf(url, sizeof(url), "%s/stroke/%s.gif", LEARN_BASE_URL, enc);
    fetch_async(url, character, true);
}

void show_page(const char* title, const char* body)
{
    if (!s_mu) return;
    lock();
    strncpy(s_title, title ? title : "", sizeof(s_title) - 1);
    strncpy(s_body, body ? body : "", sizeof(s_body) - 1);
    arm(Want::Page);
    unlock();
}

void show_formula(const char* title, const char* latex, const char* note)
{
    if (!s_mu || !latex) return;
    lock();
    strncpy(s_f_title, title ? title : "", sizeof(s_f_title) - 1);
    strncpy(s_f_src, latex, sizeof(s_f_src) - 1);
    strncpy(s_f_note, note ? note : "", sizeof(s_f_note) - 1);
    arm(Want::Formula);
    unlock();
}

void clear()
{
    if (!s_mu) return;
    lock();
    s_gen++;                                  // 在途下载一并作废
    s_visible = false;
    s_dirty = true;
    unlock();
}

bool visible()
{
    if (!s_mu) return false;
    lock(); const bool v = s_visible; unlock();
    return v;
}

}  // namespace learning
}  // namespace tdeck
