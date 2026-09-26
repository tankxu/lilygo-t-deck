// bili_app.cc — B站(搬自 stackchan-bili 的 sim/bili_ui.cpp)
//
// 三层:列表(带分区标签)→ 详情 → 播放。和模拟器一样的结构,
// 只是把 std::string/vector 换成定长结构,把 SDL 那条音频线换成设备功放。
//
// 为什么必须有台电脑转码(抄自那边的 README,免得以后有人想省掉这一步):
// ESP32-S3 上唯一可用的 H.264 软解移植自 tinyH264,只支持 constrained
// baseline;而 B站的流是 High profile + CABAC + 4 层 B 帧 —— 不是慢,是不支持。
// 所以视频在电脑上转成 MJPEG + 裸 PCM 再推过来。
//
// ⚠️ 界面里不写快捷键,全部进 shortcuts()。

#include "app.h"
#include "bili_client.h"
#include "secrets.h"
#include "ui/fonts.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

const char* TAG = "biliapp";

constexpr int SCR_W = 320, SCR_H = 240;
constexpr uint32_t C_PINK = 0xFB7299;   // B站粉
constexpr uint32_t C_TEXT = 0x1b2117;
constexpr uint32_t C_MUTE = 0x8A9480;
constexpr uint32_t C_CARD = 0xFFFFFF;
constexpr uint32_t C_SEL  = 0xFFEAF2;   // 选中行:极淡的粉

constexpr int SIDE = 10;
constexpr int TAB_H = 30;
constexpr int ROW_H  = 64;
constexpr int TH_W = 100, TH_H = 56;        // 列表缩略图(16:9)
constexpr int DT_W = 152, DT_H = 86;        // 详情页封面(16:9)

const lv_font_t* F20() { auto* f = tdeck::font_cjk();       return f ? f : &lv_font_montserrat_20; }
const lv_font_t* F16() { auto* f = tdeck::font_cjk_small(); return f ? f : &lv_font_montserrat_16; }
const lv_font_t* FICON() { return &lv_font_montserrat_16; }

enum class View { List, Detail, Player };

lv_obj_t* mk(lv_obj_t* p, const lv_font_t* f, uint32_t c, int x, int y, const char* s)
{
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t* panel(lv_obj_t* p, int x, int y, int w, int h, uint32_t bg, int r)
{
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_radius(o, r, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   // 装饰默认不抢点击
    return o;
}

// 把一帧 JPEG 解成 RGB565,【只解一遍】。
//
// ⚠️ 不能把 JPEG 直接交给 lv_image 让它在绘制时解。LVGL 9.1 的 tjpgd 是
// 流式解码器:decoder_open 只解文件头,真正的解码在 decoder_get_area 里
// 一次只做【一个 MCU】,而且不产生整帧位图(所以图片缓存根本不参与 ——
// 我一开始以为调大 LV_CACHE_DEF_SIZE 能救,实测一帧没多)。
// 再叠上 40 行的绘制缓冲把一屏切成 6 块,每块都要从头重走一遍 MCU 序列:
// 实测 320x240 只有 2.2fps。
//
// 自己调一次公开接口(open/get_area/close)把整帧解出来转成 RGB565,
// 绘制就退化成一次位图搬运,还能走 esp_lvgl_port 的 SIMD 混合。
//
// 为什么不用 ROM 里的 tjpgd(jd_prepare/jd_decomp 在 esp32s3.rom.ld 里是
// 导出的):LVGL 自带的 tjpgd.c 把同名符号定义成了【全局】,而 ld 里是
// PROVIDE —— 只在未定义时才生效,所以链接到的其实是 LVGL 那份。
// 两边的 JDEC 结构体布局又因 JD_FASTDECODE 不同而不一致,
// 拿 ROM 头去调 LVGL 的实现就是踩内存。
bool decode_rgb565(const uint8_t* jpg, int len, uint16_t* dst, int dw, int dh)
{
    lv_image_dsc_t src{};
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf    = LV_COLOR_FORMAT_RAW;
    src.header.w     = dw;
    src.header.h     = dh;
    src.data         = jpg;
    src.data_size    = len;

    lv_image_decoder_dsc_t d{};
    if (lv_image_decoder_open(&d, &src, nullptr) != LV_RESULT_OK) return false;

    lv_area_t full = { 0, 0, (int32_t)(dw - 1), (int32_t)(dh - 1) };
    lv_area_t a{};
    a.y1 = LV_COORD_MIN;          // 约定:这个值让解码器初始化内部游标
    bool ok = false;
    for (;;) {
        if (lv_image_decoder_get_area(&d, &full, &a) != LV_RESULT_OK) break;
        const lv_draw_buf_t* db = d.decoded;
        if (!db || !db->data) break;
        int mw = db->header.w, mh = db->header.h, stride = db->header.stride;
        for (int y = 0; y < mh; y++) {
            int dy = a.y1 + y;
            if (dy < 0 || dy >= dh) continue;
            const uint8_t* s = db->data + (size_t)y * stride;
            uint16_t* q = dst + (size_t)dy * dw + a.x1;
            for (int x = 0; x < mw; x++, s += 3) {
                int dx = a.x1 + x;
                if (dx < 0 || dx >= dw) continue;
                // ⚠️ 字节序是 B,G,R —— 不是 R,G,B。
                // LVGL 把 tjpgd 的输出改成了自己的 RGB888 约定(蓝在前),
                // 见 tjpgd.c:884-886 先写 B 再 G 再 R。按 R 在前读就是红蓝对调,
                // 画面整体发青。(同一张图走 LVGL 自带绘制路径是暖色,
                //  走这里变青 —— 这个对比就是证据。)
                q[x] = (uint16_t)(((s[2] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[0] >> 3));
            }
        }
        if (a.x2 >= dw - 1 && a.y2 >= dh - 1) { ok = true; break; }   // 右下角那块解完了
    }
    lv_image_decoder_close(&d);
    return ok;
}

// 播放三角:【画】出来的,不是字体图标。
//
// 原来写的是 LV_SYMBOL_PLAY —— 那是 Montserrat 私用区的一个码点,而按钮上
// 还要写"播放"两个汉字,一条标签只能指定一个字体:用中文字库渲染,那个码点
// 查不到,于是变成一个方框。换 Montserrat 又写不出汉字。
// 与其为一个三角形再塞一套字体,不如直接用 lv_draw_triangle 画,
// 和分辨率无关,想多大多大。
lv_obj_t* play_icon(lv_obj_t* parent, int s, uint32_t color)
{
    // ARGB8888 才能压在彩色按钮上而不带底。canvas 不接管缓冲的生命周期,
    // 所以挂一个 DELETE 回调自己回收 —— 否则每次重建界面漏一块。
    uint8_t* buf = (uint8_t*)lv_malloc((size_t)s * s * 4);
    if (!buf) return nullptr;
    lv_obj_t* cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, s, s, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(cv, [](lv_event_t* e) { lv_free(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, buf);
    lv_canvas_fill_bg(cv, lv_color_hex(0x000000), LV_OPA_TRANSP);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = s * 0.18f; d.p[0].y = s * 0.08f;
    d.p[1].x = s * 0.86f; d.p[1].y = s * 0.50f;
    d.p[2].x = s * 0.18f; d.p[2].y = s * 0.92f;
    d.bg_color = lv_color_hex(color);
    d.bg_opa   = LV_OPA_COVER;
    lv_draw_triangle(&layer, &d);
    lv_canvas_finish_layer(cv, &layer);
    return cv;
}

// ── 列表缩略图 ────────────────────────────────────────────
// 一行一张封面 —— 这是"像个视频 app"和"一堆文字"的分界线。
// 抓取在后台任务里逐张来(一次二十个 HTTP 会把界面卡死),解码交给 LVGL 任务:
// 解 JPEG 要走绘制管线,栈很深,抓取任务再开 16K 会把内部 RAM 压爆
// (音乐那边踩过,当时只剩 6.5KB)。
struct Thumb { uint8_t* jpg; int len; uint16_t* rgb; lv_image_dsc_t dsc; bool ready; };
Thumb   g_th[bili::LIST_MAX] = {};
Thumb   g_dth = {};                   // 详情页那一张
char    g_dth_url[200] = {};
char    g_th_url[bili::LIST_MAX][200] = {};
int     g_th_n = 0;
volatile uint32_t g_gen = 0;          // 列表重建一次就 +1,用来作废在途回调
volatile bool     g_th_stop = false;
TaskHandle_t      g_th_task = nullptr;

void thumbs_free()
{
    g_gen++;                          // 先作废,再回收
    g_th_stop = true;
    for (int i = 0; i < 200 && g_th_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    for (auto& th : g_th) {
        if (th.jpg) free(th.jpg);
        if (th.rgb) free(th.rgb);
        th = Thumb{};
    }
    g_th_stop = false;
}

void human_dur(int s, char* out, int n)
{
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s % 3600 / 60, s % 60);
    else           snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

void human_view(int64_t v, char* out, int n)
{
    if (v >= 100000000) snprintf(out, n, "%.1f 亿", v / 1e8);
    else if (v >= 10000) snprintf(out, n, "%.1f 万", v / 1e4);
    else snprintf(out, n, "%lld", (long long)v);
}

class BiliApp : public tdeck::App {
public:
    const char* name() const override    { return "BILIBILI"; }
    const char* card_title() const override { return "Bilibili"; }
    uint32_t card_color() const override { return 0xC2185B; }
    const char* icon() const override    { return LV_SYMBOL_VIDEO; }
    uint32_t accent() const override     { return C_PINK; }

    int shortcuts(const tdeck::Shortcut** out) const override
    {
        static const tdeck::Shortcut lst[] = {
            { "ball U/D",  "pick a video" },
            { "ball L/R",  "switch section" },
            { "y / click", "open" },
        };
        static const tdeck::Shortcut det[] = {
            { "y / click", "play" },
            { "n / b",     "back to list" },
        };
        static const tdeck::Shortcut ply[] = {
            { "y / click", "play / pause" },
            { "n / b",     "back" },
        };
        switch (view_) {
        case View::List:   *out = lst; return 3;
        case View::Detail: *out = det; return 2;
        case View::Player: *out = ply; return 2;
        }
        return 0;
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        self_ = this;

        bili::set_base(BILI_BASE_URL);
        if (n_reg_ == 0) n_reg_ = bili::get_regions(reg_, bili::REGION_MAX);

        // 每 20ms 轮一次:拉到的列表/详情要回贴,播放页要按音频时钟换帧。
        // 模拟器那边是主循环里直接调 bili_ui_tick(),设备上换成 LVGL 定时器 ——
        // 它就跑在 LVGL 任务里,动控件不用再加锁。
        //
        // ⚠️ 周期【必须明显小于一帧的解码时间】,否则出帧节奏被量化成周期的整数倍。
        // 原来是 40ms:解一帧 78ms,实际节奏就成了 40×2=120ms → 8.3fps,
        // 实测 7.9fps 分毫不差。改 20ms 之后量化误差从 40ms 降到 20ms。
        tick_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<BiliApp*>(lv_timer_get_user_data(t))->tick();
        }, 20, this);

        show_list(true);
    }

    void on_exit() override
    {
        bili::stream_stop();
        thumbs_free();
        if (tick_) { lv_timer_delete(tick_); tick_ = nullptr; }
        root_ = nullptr; self_ = nullptr;
        if (fb_) { free(fb_); fb_ = nullptr; }
        clear_refs();
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (view_) {
        case View::List:   return list_input(ev);
        case View::Detail: return detail_input(ev);
        case View::Player: return player_input(ev);
        }
        return false;
    }

private:
    void clear_refs()
    {
        list_ = nullptr; status_ = nullptr; play_img_ = nullptr;
        ctrl_ = nullptr; ctrl_pos_ = nullptr; stat_ = nullptr;
        for (auto& r : rows_) r = nullptr;
        for (auto& t : tabs_) t = nullptr;
        det_title_ = det_sub_ = det_desc_ = det_img_ = nullptr;
    }

    // ── 列表页 ──────────────────────────────────────────────
    void show_list(bool fetch)
    {
        view_ = View::List;
        lv_obj_clean(root_);
        clear_refs();

        mk(root_, F20(), C_PINK, SIDE, 4, "bilibili");

        // 分区标签横向排开
        lv_obj_t* bar = lv_obj_create(root_);
        lv_obj_set_size(bar, SCR_W, TAB_H);
        lv_obj_set_pos(bar, 0, 32);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(bar, SIDE, LV_PART_MAIN);
        lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
        tab_bar_ = bar;

        int x = 0;
        for (int i = 0; i < n_reg_; i++) {
            lv_obj_t* tb = panel(bar, x, 2, 56, TAB_H - 8, 0xFFFFFF, 11);
            // ⚠️ panel() 默认摘掉 CLICKABLE(它是给装饰子对象用的),标签要自己加回来。
            // 不加的话顶部菜单点不动 —— 音乐的搜索按钮、设置的行都栽过同一个坑。
            lv_obj_set_user_data(tb, (void*)(intptr_t)i);
            lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(tb, LV_OBJ_FLAG_EVENT_BUBBLE);   // 横滑手势要能穿过去
            lv_obj_add_event_cb(tb, [](lv_event_t* e) {
                auto* s = static_cast<BiliApp*>(lv_event_get_user_data(e));
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                if (idx == s->tab_) return;
                s->tab_ = idx; s->sel_ = 0; s->retab();
            }, LV_EVENT_CLICKED, this);
            lv_obj_t* l = mk(tb, F16(), C_TEXT, 0, 0, reg_[i].name);
            lv_obj_center(l);
            tabs_[i] = tb;
            x += 60;
        }
        paint_tabs();

        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W, SCR_H - 66);
        lv_obj_set_pos(list_, 0, 66);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        if (fetch) {
            n_vid_ = 0;
            status_ = mk(root_, F16(), C_MUTE, SIDE, 80, "加载中…");
            bili::request_list(reg_[tab_].rid, 1, bili::LIST_MAX);
        } else {
            build_rows();
        }
    }

    static int lh() { return F16()->line_height; }

    void build_rows()
    {
        if (!list_) return;
        lv_obj_clean(list_);
        for (auto& r : rows_) r = nullptr;
        for (auto& im : row_img_) im = nullptr;

        const int RW = SCR_W - SIDE * 2;          // 行宽 300
        const int TX = TH_W + 12;                 // 文字左边界
        const int TW = RW - TX - 8;               // 文字可用宽

        for (int i = 0; i < n_vid_; i++) {
            lv_obj_t* r = panel(list_, SIDE, i * (ROW_H + 6), RW, ROW_H, C_CARD, 12);
            lv_obj_set_user_data(r, (void*)(intptr_t)i);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);   // 竖滑手势要能穿过去
            lv_obj_add_event_cb(r, [](lv_event_t* e) {
                auto* s = static_cast<BiliApp*>(lv_event_get_user_data(e));
                s->sel_ = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                s->paint_rows();
                s->show_detail(s->vid_[s->sel_].bvid);
            }, LV_EVENT_CLICKED, this);

            // 缩略图位先占住,图抓回来再盖上去 —— 尺寸一开始就定死,
            // 免得图陆续到达时整列一跳一跳地重排。
            panel(r, 4, 4, TH_W, TH_H, 0xE6E9E0, 8);
            lv_obj_t* im = lv_image_create(r);
            lv_obj_set_pos(im, 4, 4);
            lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_radius(im, 8, LV_PART_MAIN);
            lv_obj_set_style_clip_corner(im, true, LV_PART_MAIN);
            row_img_[i] = im;

            char d[16], v[16], b[80];
            human_dur(vid_[i].dur, d, sizeof(d));
            // 时长压在封面右下角 —— 视频列表的通行做法,比挤进副标题省一行。
            lv_obj_t* db = panel(r, 4 + TH_W - 42, 4 + TH_H - 17, 38, 14, 0x000000, 4);
            lv_obj_set_style_bg_opa(db, LV_OPA_60, LV_PART_MAIN);
            lv_obj_t* dl = mk(db, &lv_font_montserrat_14, 0xFFFFFF, 0, 0, d);
            lv_obj_center(dl);

            // 标题两行。定宽【还要定高】—— LONG_DOT 在高度不受限时会先一直换行
            // 再省略,长标题会把副标题顶出行外(音乐播放页踩过)。
            lv_obj_t* tl = mk(r, F16(), C_TEXT, TX, 4, vid_[i].title);
            lv_obj_set_size(tl, TW, lh() * 2);
            lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

            human_view(vid_[i].view, v, sizeof(v));
            snprintf(b, sizeof(b), "%s · %s", vid_[i].up, v);
            lv_obj_t* sb = mk(r, F16(), C_MUTE, TX, 4 + lh() * 2, b);
            lv_obj_set_size(sb, TW, lh());
            lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);

            rows_[i] = r;
        }
        paint_rows();
        thumbs_start();
    }

    // 把抓回来的封面贴上去。由 decode 回调(跑在 LVGL 任务里)调。
    static void apply_thumb(int i)
    {
        if (!self_ || i < 0 || i >= bili::LIST_MAX) return;
        lv_obj_t* im = self_->row_img_[i];
        if (!im || !g_th[i].ready) return;
        lv_image_set_src(im, &g_th[i].dsc);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_HIDDEN);
    }

    // 详情页封面。和列表走同一条路:后台抓 JPEG,解码排到 LVGL 任务上。
    void detail_cover_start(const char* url)
    {
        if (g_dth.rgb) { free(g_dth.rgb); g_dth.rgb = nullptr; }
        if (g_dth.jpg) { free(g_dth.jpg); g_dth.jpg = nullptr; }
        g_dth.ready = false;
        snprintf(g_dth_url, sizeof(g_dth_url), "%s", url ? url : "");
        if (!g_dth_url[0]) return;
        uint32_t gen = g_gen;
        xTaskCreate([](void* a) {
            uint32_t gen = (uint32_t)(uintptr_t)a;
            int len = 0;
            uint8_t* j = bili::fetch_cover(g_dth_url, &len);
            if (j && gen == g_gen) {
                g_dth.jpg = j; g_dth.len = len;
                lvgl_port_lock(0);   // 同上:async 链表不是线程安全的
                lv_async_call([](void*) {
                    Thumb& th = g_dth;
                    if (!th.jpg) return;
                    th.rgb = (uint16_t*)heap_caps_malloc(DT_W * DT_H * 2, MALLOC_CAP_SPIRAM);
                    if (th.rgb && decode_rgb565(th.jpg, th.len, th.rgb, DT_W, DT_H)) {
                        th.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
                        th.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
                        th.dsc.header.w      = DT_W;
                        th.dsc.header.h      = DT_H;
                        th.dsc.header.stride = DT_W * 2;
                        th.dsc.data          = (const uint8_t*)th.rgb;
                        th.dsc.data_size     = DT_W * DT_H * 2;
                        th.ready = true;
                        if (self_ && self_->det_img_) {
                            lv_image_set_src(self_->det_img_, &th.dsc);
                            lv_obj_remove_flag(self_->det_img_, LV_OBJ_FLAG_HIDDEN);
                        }
                    } else if (th.rgb) { free(th.rgb); th.rgb = nullptr; }
                    free(th.jpg); th.jpg = nullptr;
                }, nullptr);
                lvgl_port_unlock();
            } else if (j) free(j);
            vTaskDelete(nullptr);
        }, "bili_dcov", 6144, (void*)(uintptr_t)gen, 3, nullptr);
    }

    void thumbs_start()
    {
        thumbs_free();
        g_th_n = n_vid_ < bili::LIST_MAX ? n_vid_ : bili::LIST_MAX;
        for (int i = 0; i < g_th_n; i++)
            snprintf(g_th_url[i], sizeof(g_th_url[i]), "%s", vid_[i].pic);
        if (g_th_n == 0) return;

        xTaskCreate([](void*) {
            uint32_t gen = g_gen;
            for (int i = 0; i < g_th_n && !g_th_stop; i++) {
                int len = 0;
                uint8_t* j = bili::fetch_cover(g_th_url[i], &len);
                if (!j) continue;
                if (gen != g_gen) { free(j); break; }      // 界面已经换过了
                g_th[i].jpg = j;
                g_th[i].len = len;
                // ⚠️ 解码排到 LVGL 任务上:那边本来就有 16K 栈,
                // 这个抓取任务只有 6K,在这儿解会直接溢出。
                // ⚠️ lv_async_call 【不是线程安全的】。我们配的是 LV_OS_NONE,
                // LVGL 内部一处锁都没有;后台任务往 async 链表挂节点的同时
                // LVGL 任务正在遍历并摘节点,链表被撕坏 —— coredump 实录:
                //   exccause 0x1c LoadProhibited, excvaddr 0x20001
                //   pc = lv_async_timer_cb+9  (lv_async.c:103)
                // 所以必须拿 LVGL 锁。这把锁就是 lvgl_port_task 跑
                // lv_timer_handler 时持有的那把,拿到就说明处理器没在跑。
                // (递归锁,从 LVGL 任务里调也不会自锁。)
                lvgl_port_lock(0);
                lv_async_call([](void* a) {
                    uint32_t packed = (uint32_t)(uintptr_t)a;
                    int idx = (int)(packed & 0xFF);
                    if ((packed >> 8) != (g_gen & 0xFFFFFF)) {
                        if (g_th[idx].jpg) { free(g_th[idx].jpg); g_th[idx].jpg = nullptr; }
                        return;
                    }
                    Thumb& th = g_th[idx];
                    if (!th.jpg) return;
                    th.rgb = (uint16_t*)heap_caps_malloc(TH_W * TH_H * 2, MALLOC_CAP_SPIRAM);
                    if (th.rgb && decode_rgb565(th.jpg, th.len, th.rgb, TH_W, TH_H)) {
                        th.dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
                        th.dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
                        th.dsc.header.w      = TH_W;
                        th.dsc.header.h      = TH_H;
                        th.dsc.header.stride = TH_W * 2;
                        th.dsc.data          = (const uint8_t*)th.rgb;
                        th.dsc.data_size     = TH_W * TH_H * 2;
                        th.ready = true;
                        BiliApp::apply_thumb(idx);
                    } else if (th.rgb) { free(th.rgb); th.rgb = nullptr; }
                    free(th.jpg); th.jpg = nullptr;
                }, (void*)(uintptr_t)((uint32_t)i | ((g_gen & 0xFFFFFF) << 8)));
                lvgl_port_unlock();
                vTaskDelay(pdMS_TO_TICKS(30));            // 让解码跟得上,别把队列灌爆
            }
            g_th_task = nullptr;
            vTaskDelete(nullptr);
        }, "bili_thumb", 6144, nullptr, 3, &g_th_task);
    }

    void paint_tabs()
    {
        for (int i = 0; i < n_reg_; i++) {
            if (!tabs_[i]) continue;
            bool on = (i == tab_);
            lv_obj_set_style_bg_color(tabs_[i], lv_color_hex(on ? C_PINK : 0xFFFFFF), LV_PART_MAIN);
            lv_obj_t* l = lv_obj_get_child(tabs_[i], 0);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xFFFFFF : C_TEXT), LV_PART_MAIN);
        }
    }

    void paint_rows()
    {
        for (int i = 0; i < n_vid_; i++) {
            if (!rows_[i]) continue;
            bool on = (i == sel_);
            // ⚠️ 边框宽度恒定,只切透明度。0->2 会把行内所有子对象往右下推
            // (LVGL 的子坐标是相对内容区的,内容区 = 外框 - 边框 - 内边距)。
            // 设置页踩过这个坑。
            lv_obj_set_style_bg_color(rows_[i], lv_color_hex(on ? C_SEL : C_CARD), LV_PART_MAIN);
            lv_obj_set_style_border_width(rows_[i], 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(rows_[i], lv_color_hex(C_PINK), LV_PART_MAIN);
            lv_obj_set_style_border_opa(rows_[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        }
        if (n_vid_ && rows_[sel_]) lv_obj_scroll_to_view(rows_[sel_], LV_ANIM_ON);
    }

    bool list_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Up:    if (sel_ > 0) { sel_--; paint_rows(); } return true;
        case tdeck::Key::Down:  if (sel_ < n_vid_ - 1) { sel_++; paint_rows(); } return true;
        case tdeck::Key::Left:  if (tab_ > 0) { tab_--; sel_ = 0; retab(); } return true;
        case tdeck::Key::Right: if (tab_ < n_reg_ - 1) { tab_++; sel_ = 0; retab(); } return true;
        case tdeck::Key::Enter:
            if (n_vid_ > 0) show_detail(vid_[sel_].bvid);
            return true;
        default: return false;
        }
    }

public:
    void retab()
    {
        paint_tabs();
        if (tab_bar_ && tabs_[tab_]) lv_obj_scroll_to_view(tabs_[tab_], LV_ANIM_ON);
        n_vid_ = 0;
        if (list_) lv_obj_clean(list_);
        for (auto& r : rows_) r = nullptr;
        if (!status_) status_ = mk(root_, F16(), C_MUTE, SIDE, 80, "加载中…");
        lv_label_set_text(status_, "加载中…");
        bili::request_list(reg_[tab_].rid, 1, bili::LIST_MAX);
    }

private:
    // ── 详情页 ──────────────────────────────────────────────
    void show_detail(const char* bvid)
    {
        view_ = View::Detail;
        snprintf(cur_bvid_, sizeof(cur_bvid_), "%s", bvid);
        lv_obj_clean(root_);
        clear_refs();

        mk(root_, F20(), C_PINK, SIDE, 4, "详情");

        // 左封面右标题 —— 原来整页只有文字,底下空掉一大块。
        panel(root_, SIDE, 34, DT_W, DT_H, 0xE6E9E0, 8);
        det_img_ = lv_image_create(root_);
        lv_obj_set_pos(det_img_, SIDE, 34);
        lv_obj_set_style_radius(det_img_, 8, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(det_img_, true, LV_PART_MAIN);
        lv_obj_add_flag(det_img_, LV_OBJ_FLAG_HIDDEN);

        const int RX = SIDE + DT_W + 10, RW = SCR_W - RX - SIDE;
        det_title_ = mk(root_, F16(), C_TEXT, RX, 34, "加载中…");
        lv_obj_set_size(det_title_, RW, lh() * 4);
        lv_label_set_long_mode(det_title_, LV_LABEL_LONG_DOT);

        det_sub_ = mk(root_, F16(), C_MUTE, SIDE, 34 + DT_H + 8, "");
        lv_obj_set_size(det_sub_, SCR_W - SIDE * 2, lh());
        lv_label_set_long_mode(det_sub_, LV_LABEL_LONG_DOT);

        det_desc_ = mk(root_, F16(), C_MUTE, SIDE, 34 + DT_H + 8 + lh() + 2, "");
        lv_obj_set_size(det_desc_, SCR_W - SIDE * 2, lh() * 2);
        lv_label_set_long_mode(det_desc_, LV_LABEL_LONG_DOT);

        lv_obj_t* btn = panel(root_, SIDE, SCR_H - 42, SCR_W - SIDE * 2, 34, C_PINK, 14);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            static_cast<BiliApp*>(lv_event_get_user_data(e))->start_play();
        }, LV_EVENT_CLICKED, this);
        // 三角是画的,不是字体图标 —— 见 play_icon() 的说明。
        lv_obj_t* bl = mk(btn, F16(), 0xFFFFFF, 0, 0, "播放");
        lv_obj_align(bl, LV_ALIGN_CENTER, 12, 0);
        if (lv_obj_t* ic = play_icon(btn, 18, 0xFFFFFF))
            lv_obj_align(ic, LV_ALIGN_CENTER, -24, 0);

        bili::request_detail(bvid);
    }

    bool detail_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Enter) { start_play(); return true; }
        if (ev.key == tdeck::Key::Back)  { show_list(false); return true; }
        return false;
    }

    // ── 播放页 ──────────────────────────────────────────────
    void start_play()
    {
        view_ = View::Player;
        lv_obj_clean(root_);
        clear_refs();
        // 画面是 16:9(服务端出 320x180),上下各露 30px。这两条用纯白 ——
        // 黑底会把屏幕的背光漏光衬得很明显,白底反而看不出来。
        lv_obj_set_style_bg_color(root_, lv_color_white(), LV_PART_MAIN);

        play_img_ = lv_image_create(root_);
        lv_obj_set_pos(play_img_, 0, 0);
        lv_obj_add_flag(play_img_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(play_img_, [](lv_event_t* e) {
            static_cast<BiliApp*>(lv_event_get_user_data(e))->toggle_ctrl();
        }, LV_EVENT_CLICKED, this);

        ctrl_ = panel(root_, 0, SCR_H - 34, SCR_W, 34, 0x000000, 0);
        lv_obj_set_style_bg_opa(ctrl_, LV_OPA_70, LV_PART_MAIN);
        ctrl_pos_ = mk(ctrl_, F16(), 0xFFFFFF, 10, 7, "缓冲中…");
        stat_ = mk(root_, FICON(), 0x9AA48C, 6, 4, "");   // 调试浮层,白底上要压得住

        // 整帧 RGB565 缓冲,进播放页时分配一次。150KB 放 PSRAM。
        if (!fb_) fb_ = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
        shown_ = 0; last_stat_ = 0; t_first_ = 0; dec_us_ = 0;
        ctrl_shown_at_ = lv_tick_get();
        bili::stream_start(cur_bvid_, 16000);
    }

    void toggle_ctrl()
    {
        if (!ctrl_) return;
        bool vis = !lv_obj_has_flag(ctrl_, LV_OBJ_FLAG_HIDDEN);
        if (vis) lv_obj_add_flag(ctrl_, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_remove_flag(ctrl_, LV_OBJ_FLAG_HIDDEN); ctrl_shown_at_ = lv_tick_get(); }
    }

    bool player_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Enter) {
            bili::stream_set_paused(!bili::stream_paused());
            if (ctrl_) { lv_obj_remove_flag(ctrl_, LV_OBJ_FLAG_HIDDEN); ctrl_shown_at_ = lv_tick_get(); }
            return true;
        }
        if (ev.key == tdeck::Key::Back) {
            bili::stream_stop();
            lv_obj_set_style_bg_color(root_, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
            show_detail(cur_bvid_);
            return true;
        }
        return false;
    }

    // 音频播到哪儿,画面就显示哪一帧 —— 音频是主时钟。
    // 落后的帧在 stream_take_due 里已经丢掉了,这里只管贴图。
    void player_tick()
    {
        const char* err;
        if (bili::stream_error(&err)) {
            if (ctrl_pos_) {
                lv_obj_remove_flag(ctrl_, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(ctrl_pos_, err);
            }
            return;
        }
        uint32_t pos = bili::stream_audio_pos_ms();
        const uint8_t* jp; int jl; uint32_t pts;
        // 画面尺寸由服务端的档位决定(现在是 320x180 —— 片源基本都是 16:9,
        // 在 320x240 里上下那两条黑边照样要花解码时间)。竖直居中放。
        bili::Meta vm = bili::stream_meta();
        int vw = vm.valid && vm.w > 0 ? vm.w : 320;
        int vh = vm.valid && vm.h > 0 ? vm.h : 240;
        if (vw > 320) vw = 320;
        if (vh > 240) vh = 240;
        if (fb_ && bili::stream_take_due(pos, &jp, &jl, &pts)) {
            int64_t t0 = esp_timer_get_time();
            if (decode_rgb565(jp, jl, fb_, vw, vh)) {
                dec_us_ = (int)(esp_timer_get_time() - t0);
                frame_dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
                frame_dsc_.header.cf     = LV_COLOR_FORMAT_RGB565;
                frame_dsc_.header.w      = vw;
                frame_dsc_.header.h      = vh;
                frame_dsc_.header.stride = vw * 2;
                frame_dsc_.data          = (const uint8_t*)fb_;
                frame_dsc_.data_size     = (uint32_t)vw * vh * 2;
                // ⚠️ 同一个 dsc 指针反复换内容,必须让 LVGL 丢掉上一帧的缓存,
                // 否则画面会永远停在第一帧(lv_ffmpeg.c 里也是这么做的)。
                lv_image_cache_drop(&frame_dsc_);
                lv_image_set_src(play_img_, &frame_dsc_);
                lv_obj_set_pos(play_img_, (320 - vw) / 2, (240 - vh) / 2);
                lv_obj_invalidate(play_img_);
                shown_++;
                if (!t_first_) t_first_ = esp_timer_get_time();
            }
        }

        uint32_t now = lv_tick_get();
        if (ctrl_ && !lv_obj_has_flag(ctrl_, LV_OBJ_FLAG_HIDDEN)
                && !bili::stream_paused() && now - ctrl_shown_at_ > 2500)
            lv_obj_add_flag(ctrl_, LV_OBJ_FLAG_HIDDEN);

        if (now - last_stat_ > 500) {
            last_stat_ = now;
            bili::Meta m = bili::stream_meta();
            char b[96], d[16];
            if (!m.valid) snprintf(b, sizeof(b), "缓冲中…");
            else {
                human_dur(m.dur, d, sizeof(d));
                snprintf(b, sizeof(b), "%02u:%02u / %s%s", (unsigned)(pos / 60000),
                         (unsigned)((pos / 1000) % 60),
                         d, bili::stream_paused() ? "  ||" : "");
            }
            if (ctrl_pos_) lv_label_set_text(ctrl_pos_, b);

            int fin, fdrop, pcm;
            bili::stream_stats(&fin, &fdrop, &pcm);
            double secs = t_first_ ? (esp_timer_get_time() - t_first_) / 1e6 : 0;
            char s[96];
            snprintf(s, sizeof(s), "in%d drop%d show%d %.1ffps dec%dms buf%.1fs",
                     fin, fdrop, shown_, secs > 0.5 ? shown_ / secs : 0.0,
                     dec_us_ / 1000, pcm / 32000.0);
            if (stat_) lv_label_set_text(stat_, s);
        }
    }

    void tick()
    {
        if (view_ == View::Player) { player_tick(); return; }

        if (view_ == View::List) {
            int n, rid, pn; bool more;
            if (bili::poll_list(vid_, &n, &rid, &pn, &more)) {
                n_vid_ = n; sel_ = 0;
                if (status_) { lv_obj_delete(status_); status_ = nullptr; }
                build_rows();
            }
            const char* e;
            if (bili::list_failed(&e) && status_) lv_label_set_text(status_, e);
            return;
        }

        if (view_ == View::Detail) {
            bili::Detail d;
            if (bili::poll_detail(&d)) {
                det_ = d;
                if (det_title_) lv_label_set_text(det_title_, d.title);
                char b[96], v[16], du[16];
                human_view(d.view, v, sizeof(v));
                human_dur(d.dur, du, sizeof(du));
                snprintf(b, sizeof(b), "%s · %s 播放 · %s", d.up, v, du);
                if (det_sub_) lv_label_set_text(det_sub_, b);
                if (det_desc_) lv_label_set_text(det_desc_, d.desc);
                detail_cover_start(d.pic);
            }
            const char* e;
            if (bili::detail_failed(&e) && det_title_) lv_label_set_text(det_title_, e);
        }
    }

    lv_obj_t* root_ = nullptr, *list_ = nullptr, *tab_bar_ = nullptr, *status_ = nullptr;
    lv_obj_t* rows_[bili::LIST_MAX] = {};
    lv_obj_t* row_img_[bili::LIST_MAX] = {};
    lv_obj_t* tabs_[bili::REGION_MAX] = {};
    lv_obj_t* det_title_ = nullptr, *det_sub_ = nullptr, *det_desc_ = nullptr, *det_img_ = nullptr;
    lv_obj_t* play_img_ = nullptr, *ctrl_ = nullptr, *ctrl_pos_ = nullptr, *stat_ = nullptr;
    lv_timer_t* tick_ = nullptr;

    lv_image_dsc_t frame_dsc_{};
    bili::Region reg_[bili::REGION_MAX] = {};
    bili::Video  vid_[bili::LIST_MAX] = {};
    bili::Detail det_{};
    char cur_bvid_[20] = {};
    int  n_reg_ = 0, n_vid_ = 0, tab_ = 0, sel_ = 0;
    uint16_t* fb_ = nullptr;
    int  shown_ = 0, dec_us_ = 0;
    uint32_t last_stat_ = 0, ctrl_shown_at_ = 0;
    int64_t  t_first_ = 0;
    View view_ = View::List;

    static inline BiliApp* self_ = nullptr;
};

}  // namespace

TDECK_REGISTER_APP(BiliApp)
