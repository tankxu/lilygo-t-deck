// selftest_app.cc — 硬件自检
//
// 设计原则只有一条:**每一项都必须由人的动作产生证据才算通过,不读寄存器交差。**
//
// "I2C 能 ping 到 0x55" 只证明键盘 MCU 活着,不证明第 3 排第 5 个键的触点没氧化;
// "ES7210 初始化返回 ESP_OK" 不证明麦克风膜片没被摔坏。所以每一站都是一个
// 必须做完才能通过的小游戏 —— 按遍每个键、把光标推进四个角、对着话筒说话。
//
// 这也是 BSP 的验收夹具:每写完一个驱动,自检里就多亮一站。
// 完整规格见 docs/apps/selftest.md。

#include "app.h"
#include "ui/fonts.h"
#include "tdeck_bsp.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// 卡片配图。tools/gen_card_art.py 生成,CMake EMBED_FILES 以二进制嵌入
// (143x94 RGB565,26884 字节),不走 C 数组。
extern "C" const uint8_t card_selftest_start[] asm("_binary_card_selftest_rgb565_start");
const lv_image_dsc_t kCardArt = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = 143, .h = 94, .stride = 143 * 2, .reserved_2 = 0 },
    .data_size = 143 * 94 * 2,
    .data      = card_selftest_start,
};

const char* TAG = "selftest";

constexpr int SCR_W = 320, SCR_H = 240;
constexpr int CJK_LINE_H = 26;

constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_TEXT   = 0x1B2117;
constexpr uint32_t C_MUTE   = 0x8A9480;
constexpr uint32_t C_FAIL   = 0xB4462F;
constexpr uint32_t C_CARD   = 0xFFFFFF;
constexpr uint32_t C_IDLE   = 0xE8EEE0;

inline const lv_font_t* F16() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_16; }
inline const lv_font_t* F20() { auto* f = tdeck::font_cjk(); return f ? f : &lv_font_montserrat_20; }
// 图标是 FontAwesome 私用区码位,中文字库里没有,一律用 Montserrat
inline const lv_font_t* FICON() { return &lv_font_montserrat_16; }

enum class Result : uint8_t { Untested, Pass, Fail, Skip };

struct Station {
    const char* name;
    const char* hint;      // 首页卡片上的一句话说明
    Result      result = Result::Untested;
    char        detail[40] = {};
};

// 站的顺序就是排查顺序:先证明输入能用,再证明音频能用,最后看屏幕。
enum StId { ST_KEY = 0, ST_BALL, ST_TOUCH, ST_MIC, ST_SPK, ST_LCD, ST_N };

Station g_st[ST_N] = {
    { "键盘",   "按遍每个键" },
    { "轨迹球", "推光标进四角 + 中键" },
    { "触摸",   "划到四角 + 长按一次" },
    { "麦克风", "说话让电平过线 1 秒" },
    { "喇叭",   "扫频 + 回放刚录的声音" },
    { "屏幕",   "纯色 / 灰阶 / 坏点" },
};

// 麦克风那一站录下来的 3 秒,给喇叭那一站回放。
// 这段闭环(麦克风 → I2S → PSRAM → I2S → 功放)一次跑通,
// 就等于整条音频链路通了 —— 这正是小智能不能跑的前提。
constexpr int      REC_RATE = 16000;   // int 不用 uint32_t:下面到处拿它做整数算式,
                                       // 无符号会把表达式整体提升成 unsigned long,
                                       // printf 的 %d 直接报 -Werror=format
constexpr int      REC_SECS = 3;
int16_t*  g_rec      = nullptr;
int       g_rec_len  = 0;     // 已录的样本数

const char* result_text(Result r)
{
    switch (r) {
    case Result::Pass: return "通过";
    case Result::Fail: return "失败";
    case Result::Skip: return "跳过";
    default:           return "未测";
    }
}

uint32_t result_color(Result r)
{
    switch (r) {
    case Result::Pass: return C_ACCENT;
    case Result::Fail: return C_FAIL;
    case Result::Skip: return C_MUTE;
    default:           return 0xB6C2A6;
    }
}

// ── 通用小控件 ────────────────────────────────────────────
lv_obj_t* lbl(lv_obj_t* p, const lv_font_t* f, uint32_t c, int x, int y, const char* s)
{
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t* box(lv_obj_t* p, int x, int y, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

class SelfTestApp : public tdeck::App {
public:
    const char* name() const override   { return "SELFTEST"; }
    const lv_image_dsc_t* card_art() const override  { return &kCardArt; }
    const char* card_title() const override           { return "Self Test"; }
    const char* icon() const override   { return LV_SYMBOL_SETTINGS; }
    uint32_t    accent() const override { return C_ACCENT; }

    // 键盘那一站要收原始字符,别让 launcher 把 q/s/y/n 截走。
    // 其它站不要 —— 否则连退出都得靠 ESC,手感很别扭。
    bool wants_raw_keys() const override { return view_ == ST_KEY; }

    int shortcuts(const tdeck::Shortcut** out) const override
    {
        static const tdeck::Shortcut home[] = {
            { "y / click", "run this test" },
            { "ball U/D",  "pick test" },
        };
        static const tdeck::Shortcut key[] = {
            { "any key",   "light it up" },
            { "back btn",  "leave (no ESC on this keyboard)" },
        };
        static const tdeck::Shortcut ball[] = {
            { "ball",      "move cursor into corners" },
            { "click",     "middle button check" },
        };
        static const tdeck::Shortcut touch[] = {
            { "drag",      "hit all four corners" },
            { "long press","press-and-hold check" },
        };
        static const tdeck::Shortcut spk[] = {
            { "y / click", "play, then confirm" },
            { "n",         "did not hear it" },
        };
        static const tdeck::Shortcut lcd[] = {
            { "left/right","change pattern" },
            { "y",         "no dead pixels" },
            { "n",         "found a defect" },
        };
        switch (view_) {
        case -1:       *out = home;  return 2;
        case ST_KEY:   *out = key;   return 2;
        case ST_BALL:  *out = ball;  return 2;
        case ST_TOUCH: *out = touch; return 2;
        case ST_SPK:   *out = spk;   return 2;
        case ST_LCD:   *out = lcd;   return 3;
        default:       return 0;
        }
    }

    void render_card(lv_obj_t* card) override
    {
        // ⚠️ 卡片尺寸写死 143×94,【不能】用 lv_obj_get_width(card) 去问 ——
        // render_card 是在卡片刚建出来、布局还没算过的时候调的,那时宽高都是 0,
        // 算出来的坐标全是负数,画出来就是一张空卡。
        const int W = 143, H = 94;
        lv_obj_set_style_bg_color(card, lv_color_hex(0xF3F6EF), LV_PART_MAIN);

        // 一张"检查清单":六格,前四格已完成(实心),后两格待测(描边)。
        // 不画图标 + 标题 + 说明那一套 —— 卡片是一个内容整体,
        // 一眼看出"这是逐项打勾的东西"就够了。
        const int TW = 30, TH = 18, GX = 9, GY = 10;
        int x0 = (W - (3 * TW + 2 * GX)) / 2;
        for (int i = 0; i < 6; i++) {
            bool done = (i < 4);
            lv_obj_t* b = box(card, x0 + (i % 3) * (TW + GX), 16 + (i / 3) * (TH + GY),
                              TW, TH, done ? C_ACCENT : 0xFFFFFF, 6);
            if (!done) {
                lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
                lv_obj_set_style_border_color(b, lv_color_hex(0xC8D3BA), LV_PART_MAIN);
            }
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* nm = lv_label_create(card);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nm, lv_color_hex(0x1B2117), LV_PART_MAIN);
        lv_label_set_text(nm, "Self Test");
        lv_obj_set_pos(nm, 12, H - 26);
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        self_ = this;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        show_home();
    }

    void on_exit() override
    {
        stop_mic();
        kill_timer();
        root_ = nullptr;
        self_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (view_) {
        case -1:      return home_input(ev);
        case ST_KEY:  return key_input(ev);
        case ST_BALL: return ball_input(ev);
        case ST_TOUCH:return touch_input(ev);
        case ST_MIC:  return mic_input(ev);
        case ST_SPK:  return spk_input(ev);
        case ST_LCD:  return lcd_input(ev);
        }
        return false;
    }

private:
    // ══ 首页:六张站点卡 ══════════════════════════════════
    //
    // 首页同时就是汇总页(规格里的第 7 站):每张卡右边挂着自己的结论,
    // 选中回车就重测那一站。做成两个界面只会让"上次测到哪了"分散在两处。
    static constexpr int HDR_H = 34, CARD_X = 12, CARD_H = 40, CARD_GAP = 6;
    static constexpr int CARD_W = SCR_W - CARD_X * 2;

    void show_home()
    {
        view_ = -1;
        kill_timer();
        stop_mic();
        lv_obj_clean(root_);
        memset(cards_, 0, sizeof(cards_));

        lbl(root_, F20(), C_TEXT, 14, (HDR_H - CJK_LINE_H) / 2, "自检");

        int pass = 0;
        for (auto& s : g_st) if (s.result == Result::Pass) pass++;
        char sum[24];
        snprintf(sum, sizeof(sum), "%d/%d", pass, ST_N);
        lbl(root_, &lv_font_montserrat_14, C_MUTE, 62, (HDR_H - 18) / 2, sum);

        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W, SCR_H - HDR_H);
        lv_obj_set_pos(list_, 0, HDR_H);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < ST_N; i++) {
            lv_obj_t* c = box(list_, CARD_X, i * (CARD_H + CARD_GAP),
                              CARD_W, CARD_H, C_CARD, 12);
            lv_obj_set_style_shadow_width(c, 8, LV_PART_MAIN);
            lv_obj_set_style_shadow_offset_y(c, 2, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(c, LV_OPA_10, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(c, lv_color_hex(C_ACCENT), LV_PART_MAIN);
            lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT), LV_PART_MAIN);
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(c, (void*)(intptr_t)i);
            lv_obj_add_event_cb(c, [](lv_event_t* e) {
                auto* s = static_cast<SelfTestApp*>(lv_event_get_user_data(e));
                int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
                s->sel_ = idx;
                s->enter_station(idx);
            }, LV_EVENT_CLICKED, this);

            // 卡片上【只放站名和结论】。那句提示是中文,而中文字库只有 20px
            // 一个字号、行高 26 —— 名字加提示两行就是 52,卡片得做到 58 高,
            // 六张一屏连一半都看不到。提示挪到进站之后的标题栏里,
            // 那里本来就有一整行位置。
            // (别拿 montserrat_14 画中文:它没有汉字字形,出来是一排方框。)
            lbl(c, F16(), C_TEXT, 12, (CARD_H - CJK_LINE_H) / 2, g_st[i].name);
            if (g_st[i].detail[0])
                lbl(c, &lv_font_montserrat_14, C_MUTE, 92, (CARD_H - 18) / 2, g_st[i].detail);

            // 结论做成右边一颗药丸,颜色就是结论本身,不用看字也能扫
            lv_obj_t* pill = box(c, CARD_W - 68, (CARD_H - 22) / 2, 56, 22,
                                 result_color(g_st[i].result), 11);
            lv_obj_set_style_bg_opa(pill,
                g_st[i].result == Result::Untested ? LV_OPA_30 : LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_t* pl = lbl(pill, F16(),
                g_st[i].result == Result::Untested ? C_MUTE : 0xFFFFFF,
                0, 0, result_text(g_st[i].result));
            lv_obj_center(pl);
            cards_[i] = c;
        }
        paint_home();
    }

    void paint_home()
    {
        for (int i = 0; i < ST_N; i++) {
            if (!cards_[i]) continue;
            bool on = (i == sel_);
            lv_obj_set_style_border_width(cards_[i], on ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(cards_[i], on ? LV_OPA_30 : LV_OPA_10, LV_PART_MAIN);
        }
        if (cards_[sel_]) lv_obj_scroll_to_view(cards_[sel_], LV_ANIM_ON);
    }

    bool home_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Up:    sel_ = (sel_ + ST_N - 1) % ST_N; paint_home(); return true;
        case tdeck::Key::Down:  sel_ = (sel_ + 1) % ST_N;        paint_home(); return true;
        case tdeck::Key::Enter: enter_station(sel_);             return true;
        default: return false;
        }
    }

    void enter_station(int id)
    {
        lv_obj_clean(root_);
        kill_timer();
        view_ = id;
        switch (id) {
        case ST_KEY:   show_key();   break;
        case ST_BALL:  show_ball();  break;
        case ST_TOUCH: show_touch(); break;
        case ST_MIC:   show_mic();   break;
        case ST_SPK:   show_spk();   break;
        case ST_LCD:   show_lcd();   break;
        }
    }

    // 每一站顶上都是同一条:站名 + 一句提示 + 退出方式。
    // 位置固定,眼睛不用重新找。
    lv_obj_t* station_header(const char* title, const char* hint)
    {
        lbl(root_, F20(), C_TEXT, 14, 4, title);

        // ⚠️ 返回必须做成【能点的按钮】,不能只靠 ESC。
        // T-Deck 的 BBQ10 键盘上【没有 ESC 键】,而键盘那一站又声明了
        // wants_raw_keys(所有字母都得原样收下,否则测不了 q/s/y/n),
        // 于是键盘和轨迹球都出不去 —— 只剩触摸这一条路。
        // 顺带也符合"界面上放控件、不放快捷键提示"。
        lv_obj_t* b = box(root_, SCR_W - 44, 6, 34, 26, C_IDLE, 13);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto* s = static_cast<SelfTestApp*>(lv_event_get_user_data(e));
            s->finish(g_st[s->view_].result == Result::Pass ? Result::Pass : Result::Skip);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* bi = lbl(b, FICON(), C_ACCENT, 0, 0, LV_SYMBOL_LEFT);
        lv_obj_center(bi);

        return lbl(root_, F16(), C_MUTE, 14, 32, hint);
    }

    void finish(Result r, const char* detail = nullptr)
    {
        g_st[view_].result = r;
        if (detail) snprintf(g_st[view_].detail, sizeof(g_st[view_].detail), "%s", detail);
        else        g_st[view_].detail[0] = 0;
        show_home();
    }

    // ══ 1. 键盘 ══════════════════════════════════════════
    //
    // 画出 T-Deck 的 BBQ10 布局,按到哪个亮哪个。
    // 抓的是单键失效、连键、串扰(按 A 却亮了 S)—— 这些只有真按才看得出来。
    static constexpr char KROW0[] = "QWERTYUIOP";
    static constexpr char KROW1[] = "ASDFGHJKL";
    static constexpr char KROW2[] = "ZXCVBNM";

    void show_key()
    {
        memset(khit_, 0, sizeof(khit_));
        kcount_ = 0;
        memset(kobj_, 0, sizeof(kobj_));

        station_header("键盘", "按遍每一个键");

        const int KW = 28, KH = 24, GAP = 2;
        auto row = [&](const char* s, int y, int x0) {
            for (int i = 0; s[i]; i++) {
                lv_obj_t* b = box(root_, x0 + i * (KW + GAP), y, KW, KH, C_IDLE, 5);
                lv_obj_t* l = lbl(b, &lv_font_montserrat_14, C_MUTE, 0, 0, "");
                lv_label_set_text_fmt(l, "%c", s[i]);
                lv_obj_center(l);
                kobj_[key_slot(s[i])] = b;
            }
        };
        row(KROW0, 58, 10);
        row(KROW1, 86, 25);
        row(KROW2, 114, 40);

        // 空格 / 回车 / 退格:三个宽键
        struct { const char* t; char c; int x, w; } wide[] = {
            { "SPACE", ' ',  10, 120 },
            { "ENTER", '\r', 136, 84 },
            { "DEL",   '\b', 224, 86 },
        };
        for (auto& w : wide) {
            lv_obj_t* b = box(root_, w.x, 142, w.w, 24, C_IDLE, 5);
            lv_obj_t* l = lbl(b, &lv_font_montserrat_14, C_MUTE, 0, 0, w.t);
            lv_obj_center(l);
            kobj_[key_slot(w.c)] = b;
        }

        kecho_ = lbl(root_, F20(), C_ACCENT, 0, 176, " ");
        lv_obj_set_width(kecho_, SCR_W);
        lv_obj_set_style_text_align(kecho_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        kprog_ = lbl(root_, &lv_font_montserrat_14, C_MUTE, 0, 212, "");
        lv_obj_set_width(kprog_, SCR_W);
        lv_obj_set_style_text_align(kprog_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        update_key_prog();
    }

    // 把字符映射到槽位。只收录我们画出来的那些键,其它一律 -1。
    static int key_slot(char c)
    {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        for (int i = 0; KROW0[i]; i++) if (KROW0[i] == c) return i;
        for (int i = 0; KROW1[i]; i++) if (KROW1[i] == c) return 10 + i;
        for (int i = 0; KROW2[i]; i++) if (KROW2[i] == c) return 19 + i;
        if (c == ' ')  return 26;
        if (c == '\r' || c == '\n') return 27;
        if (c == '\b' || c == 127)  return 28;
        return -1;
    }
    static constexpr int KSLOTS = 29;

    void update_key_prog()
    {
        if (kprog_) lv_label_set_text_fmt(kprog_, "%d / %d", kcount_, KSLOTS);
    }

    bool key_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { finish(kcount_ >= KSLOTS ? Result::Pass : Result::Skip); return true; }

        char c = 0;
        if (ev.key == tdeck::Key::Char)  c = ev.ch;
        else if (ev.key == tdeck::Key::Enter) c = '\r';
        if (!c) return true;

        if (kecho_) {
            char s[8];
            if (c == ' ')       snprintf(s, sizeof(s), "SPACE");
            else if (c == '\r') snprintf(s, sizeof(s), "ENTER");
            else if (c == '\b' || c == 127) snprintf(s, sizeof(s), "DEL");
            else                snprintf(s, sizeof(s), "%c", c);
            lv_label_set_text(kecho_, s);
        }

        int k = key_slot(c);
        if (k >= 0 && !khit_[k]) {
            khit_[k] = true;
            kcount_++;
            if (kobj_[k]) {
                lv_obj_set_style_bg_color(kobj_[k], lv_color_hex(C_ACCENT), LV_PART_MAIN);
                lv_obj_t* l = lv_obj_get_child(kobj_[k], 0);
                if (l) lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
            update_key_prog();
            if (kcount_ >= KSLOTS) {
                char d[40];
                snprintf(d, sizeof(d), "%d/%d 键", kcount_, KSLOTS);
                finish(Result::Pass, d);
            }
        }
        return true;
    }

    // ══ 2. 轨迹球 ════════════════════════════════════════
    //
    // 把光标推进四个角的靶区。这比"打印 GPIO 电平"强的地方在于:
    // 它同时验证了方向映射有没有接反 —— 往左推光标却往右跑,靶区就点不亮。
    void show_ball()
    {
        memset(bhit_, 0, sizeof(bhit_));
        bclick_ = false;
        station_header("轨迹球", "把光标推进四个角,再按一下中键");

        const int T = 44;
        const int px[4] = { 8, SCR_W - T - 8, 8, SCR_W - T - 8 };
        const int py[4] = { 62, 62, SCR_H - T - 10, SCR_H - T - 10 };
        for (int i = 0; i < 4; i++) {
            btgt_[i] = box(root_, px[i], py[i], T, T, C_IDLE, 10);
            bx_[i] = px[i]; by_[i] = py[i];
        }
        bcur_x_ = SCR_W / 2; bcur_y_ = 150;
        bcur_ = box(root_, bcur_x_ - 8, bcur_y_ - 8, 16, 16, C_ACCENT, 8);

        bprog_ = lbl(root_, &lv_font_montserrat_14, C_MUTE, 0, 212, "");
        lv_obj_set_width(bprog_, SCR_W);
        lv_obj_set_style_text_align(bprog_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        paint_ball();
    }

    void paint_ball()
    {
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (bhit_[i]) n++;
            if (btgt_[i])
                lv_obj_set_style_bg_color(btgt_[i],
                    lv_color_hex(bhit_[i] ? C_ACCENT : C_IDLE), LV_PART_MAIN);
        }
        if (bcur_) lv_obj_set_pos(bcur_, bcur_x_ - 8, bcur_y_ - 8);
        if (bprog_)
            lv_label_set_text_fmt(bprog_, "%d / 4   %s", n, bclick_ ? "click OK" : "click --");
        if (n == 4 && bclick_) finish(Result::Pass);
    }

    bool ball_input(const tdeck::InputEvent& ev)
    {
        const int STEP = 14, T = 44;
        switch (ev.key) {
        case tdeck::Key::Up:    bcur_y_ -= STEP; break;
        case tdeck::Key::Down:  bcur_y_ += STEP; break;
        case tdeck::Key::Left:  bcur_x_ -= STEP; break;
        case tdeck::Key::Right: bcur_x_ += STEP; break;
        case tdeck::Key::Enter: bclick_ = true;  break;
        case tdeck::Key::Back:  finish(Result::Skip); return true;
        default: return false;
        }
        if (bcur_x_ < 8) bcur_x_ = 8;
        if (bcur_x_ > SCR_W - 8) bcur_x_ = SCR_W - 8;
        if (bcur_y_ < 56) bcur_y_ = 56;
        if (bcur_y_ > SCR_H - 8) bcur_y_ = SCR_H - 8;

        for (int i = 0; i < 4; i++) {
            if (bcur_x_ >= bx_[i] && bcur_x_ <= bx_[i] + T &&
                bcur_y_ >= by_[i] && bcur_y_ <= by_[i] + T) bhit_[i] = true;
        }
        paint_ball();
        return true;
    }

    // ══ 3. 触摸 ══════════════════════════════════════════
    //
    // 手指划过留下轨迹。四角各一个热区必须都划到 —— GT911 没校准时
    // 边缘最容易飘,中间准不代表边上准。再加一次长按,验证按压时长判定。
    void show_touch()
    {
        memset(thit_, 0, sizeof(thit_));
        tlong_ = false;
        tdots_ = 0;
        station_header("触摸", "划过四个角,再长按一下");

        tpad_ = box(root_, 0, 56, SCR_W, SCR_H - 56, 0xFBFCF9, 0);
        lv_obj_add_flag(tpad_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tpad_, [](lv_event_t* e) {
            static_cast<SelfTestApp*>(lv_event_get_user_data(e))->on_touch(e);
        }, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(tpad_, [](lv_event_t* e) {
            static_cast<SelfTestApp*>(lv_event_get_user_data(e))->on_touch(e);
        }, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(tpad_, [](lv_event_t* e) {
            auto* s = static_cast<SelfTestApp*>(lv_event_get_user_data(e));
            s->tlong_ = true; s->paint_touch();
        }, LV_EVENT_LONG_PRESSED, this);

        const int T = 40;
        const int px[4] = { 0, SCR_W - T, 0, SCR_W - T };
        const int py[4] = { 0, 0, SCR_H - 56 - T, SCR_H - 56 - T };
        for (int i = 0; i < 4; i++) {
            ttgt_[i] = box(tpad_, px[i], py[i], T, T, C_IDLE, 0);
            tx_[i] = px[i]; ty_[i] = py[i];
        }
        tprog_ = lbl(root_, &lv_font_montserrat_14, C_MUTE, SCR_W - 150, 32, "");
        paint_touch();
    }

    void on_touch(lv_event_t* e)
    {
        lv_indev_t* indev = lv_event_get_indev(e);
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int lx = p.x, ly = p.y - 56;          // 转成 tpad_ 内的坐标
        if (ly < 0) return;

        const int T = 40;
        for (int i = 0; i < 4; i++)
            if (lx >= tx_[i] && lx <= tx_[i] + T && ly >= ty_[i] && ly <= ty_[i] + T)
                thit_[i] = true;

        // 轨迹点。数量封顶,超了就从头覆写 —— 划久了不至于把内存画满。
        constexpr int MAXD = 120;
        if (tdots_ < MAXD) {
            lv_obj_t* d = box(tpad_, lx - 3, ly - 3, 6, 6, C_ACCENT, 3);
            lv_obj_set_style_bg_opa(d, LV_OPA_70, LV_PART_MAIN);
            tdot_[tdots_ % MAXD] = d;
            tdots_++;
        } else {
            lv_obj_t* d = tdot_[tdots_ % MAXD];
            if (d) lv_obj_set_pos(d, lx - 3, ly - 3);
            tdots_++;
        }
        paint_touch();
    }

    void paint_touch()
    {
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (thit_[i]) n++;
            if (ttgt_[i])
                lv_obj_set_style_bg_color(ttgt_[i],
                    lv_color_hex(thit_[i] ? C_ACCENT : C_IDLE), LV_PART_MAIN);
        }
        if (tprog_)
            lv_label_set_text_fmt(tprog_, "%d / 4   %s", n, tlong_ ? "long OK" : "long ?");
        if (n == 4 && tlong_) finish(Result::Pass);
    }

    bool touch_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { finish(Result::Skip); return true; }
        return false;
    }

    // ══ 4. 麦克风 ════════════════════════════════════════
    //
    // 实时电平条。要求电平过线并【维持 1 秒】才算过 —— 一次瞬时尖峰
    // 可能只是电源噪声,持续的声音才证明膜片在工作。
    // 过线之后自动录 3 秒,留给下一站做端到端回放。
    static constexpr int MIC_GATE = 1200;    // RMS 阈值(16bit 满幅 32767)

    void show_mic()
    {
        station_header("麦克风", "对着说话,让电平过线并保持 1 秒");

        mvu_bg_  = box(root_, 20, 96, SCR_W - 40, 26, C_IDLE, 13);
        mvu_     = box(root_, 20, 96, 2, 26, C_ACCENT, 13);
        // 阈值线:让人看得见"要推到哪"
        box(root_, 20 + (SCR_W - 40) * MIC_GATE / 8000, 90, 2, 38, 0xC46B4F, 1);

        mtxt_  = lbl(root_, F16(), C_TEXT, 20, 134, "启动中…");
        mstat_ = lbl(root_, &lv_font_montserrat_14, C_MUTE, 20, 168, "");

        mrec_bg_ = box(root_, 20, 196, SCR_W - 40, 8, C_IDLE, 4);
        mrec_    = box(root_, 20, 196, 0, 8, 0x8AA76B, 4);

        if (!g_rec) {
            g_rec = (int16_t*)heap_caps_malloc(REC_RATE * REC_SECS * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM);
        }
        g_rec_len = 0;
        mhold_ms_ = 0;
        mpeak_ = 0;

        if (tdeck_mic_init(REC_RATE) != ESP_OK || tdeck_mic_start() != ESP_OK) {
            lv_label_set_text(mtxt_, "麦克风起不来");
            finish_later(Result::Fail, "init 失败");
            return;
        }
        mic_on_ = true;
        start_timer(60, [](lv_timer_t* t) {
            static_cast<SelfTestApp*>(lv_timer_get_user_data(t))->mic_tick();
        });
    }

    void mic_tick()
    {
        // 一次读一小块。这是在 LVGL 定时器里做的,不能读太久 ——
        // 读满 60ms 的量就走,界面才不会卡。
        constexpr int N = 512;
        static int16_t buf[N];
        int got = tdeck_mic_read(buf, N, 20);
        if (got <= 0) return;

        int64_t acc = 0;
        for (int i = 0; i < got; i++) acc += (int64_t)buf[i] * buf[i];
        int rms = (int)sqrt((double)acc / got);
        if (rms > mpeak_) mpeak_ = rms;

        int w = (SCR_W - 40) * rms / 8000;
        if (w > SCR_W - 40) w = SCR_W - 40;
        if (w < 2) w = 2;
        if (mvu_) lv_obj_set_width(mvu_, w);

        if (mstat_) lv_label_set_text_fmt(mstat_, "RMS %d   peak %d", rms, mpeak_);

        if (rms >= MIC_GATE) mhold_ms_ += 60; else mhold_ms_ = 0;

        if (mhold_ms_ < 1000) {
            if (mtxt_) lv_label_set_text_fmt(mtxt_, "过线保持 %d ms", mhold_ms_);
            return;
        }

        // 过线了,开始录 3 秒留给喇叭那一站
        if (g_rec && g_rec_len < REC_RATE * REC_SECS) {
            int room = REC_RATE * REC_SECS - g_rec_len;
            int n = got < room ? got : room;
            memcpy(g_rec + g_rec_len, buf, n * sizeof(int16_t));
            g_rec_len += n;
            if (mrec_)
                lv_obj_set_width(mrec_, (SCR_W - 40) * g_rec_len / (REC_RATE * REC_SECS));
            if (mtxt_)
                lv_label_set_text_fmt(mtxt_, "录音中 %d%%",
                                      g_rec_len * 100 / (REC_RATE * REC_SECS));
            return;
        }

        char d[40];
        snprintf(d, sizeof(d), "峰值 RMS %d", mpeak_);
        stop_mic();
        finish(Result::Pass, d);
    }

    bool mic_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) {
            char d[40];
            snprintf(d, sizeof(d), "峰值 RMS %d", mpeak_);
            stop_mic();
            // 一路静默 = 真的坏了,不是"没测";这时候记 FAIL 才有意义
            finish(mpeak_ < 60 ? Result::Fail : Result::Skip, d);
            return true;
        }
        return false;
    }

    void stop_mic()
    {
        if (mic_on_) { tdeck_mic_stop(); mic_on_ = false; }
    }

    // ══ 5. 喇叭 ══════════════════════════════════════════
    //
    // 第二步是整个自检里最有价值的一项:回放刚才录的 3 秒,
    // 走的是 麦克风 → I2S → PSRAM → I2S → 功放 的端到端闭环。
    // 这条通了就等于整条音频链路通了 —— 小智能不能跑的前提。
    void show_spk()
    {
        station_header("喇叭", "先放扫频,再回放刚才录的声音");
        spk_step_ = 0;
        stxt_ = lbl(root_, F16(), C_TEXT, 20, 96, "");
        lv_obj_set_width(stxt_, SCR_W - 40);
        shint_ = lbl(root_, &lv_font_montserrat_14, C_MUTE, 20, 170, "");
        paint_spk();
    }

    void paint_spk()
    {
        const char* t[] = {
            "1/2  扫频 440 → 1k → 4k Hz",
            "2/2  回放刚才录的 3 秒",
            "听到了吗?  y 通过 / n 失败",
        };
        if (stxt_) lv_label_set_text(stxt_, t[spk_step_ < 2 ? spk_step_ : 2]);
        if (shint_) {
            if (spk_step_ == 1 && g_rec_len == 0)
                lv_label_set_text(shint_, "没有录音 —— 先过麦克风那一站");
            else
                lv_label_set_text(shint_, "");
        }
    }

    bool spk_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { finish(Result::Skip); return true; }
        if (ev.key == tdeck::Key::Char && (ev.ch == 'y' || ev.ch == 'Y')) {
            finish(Result::Pass); return true;
        }
        if (ev.key == tdeck::Key::Char && (ev.ch == 'n' || ev.ch == 'N')) {
            finish(Result::Fail, "听不到"); return true;
        }
        if (ev.key != tdeck::Key::Enter) return false;

        if (spk_step_ == 0)      { play_sweep(); spk_step_ = 1; }
        else if (spk_step_ == 1) { play_record(); spk_step_ = 2; }
        paint_spk();
        return true;
    }

    void play_sweep()
    {
        tdeck_audio_init(REC_RATE, 1);
        const int freqs[] = { 440, 1000, 4000 };
        constexpr int CH = 256;
        static int16_t t[CH];
        for (int f : freqs) {
            int total = REC_RATE * 300 / 1000;     // 每段 300ms
            double ph = 0, step = 2.0 * M_PI * f / REC_RATE;
            for (int done = 0; done < total; done += CH) {
                int n = (total - done) < CH ? (total - done) : CH;
                for (int i = 0; i < n; i++) { t[i] = (int16_t)(8000 * sin(ph)); ph += step; }
                tdeck_speaker_write(t, n, 500);
            }
        }
    }

    void play_record()
    {
        if (!g_rec || g_rec_len == 0) return;
        tdeck_audio_init(REC_RATE, 1);
        tdeck_speaker_write(g_rec, g_rec_len, 4000);
    }

    // ══ 6. 屏幕 ══════════════════════════════════════════
    void show_lcd()
    {
        lcd_page_ = 0;
        lcd_pad_ = box(root_, 0, 0, SCR_W, SCR_H, 0xFF0000, 0);
        lcd_txt_ = lbl(root_, &lv_font_montserrat_14, 0xFFFFFF, 10, SCR_H - 24, "");
        paint_lcd();
    }

    void paint_lcd()
    {
        // 灰阶那一页单独拼 16 条,其余是纯色。坏点在纯色上最好找。
        static const uint32_t colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000 };
        static const char* names[] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK", "GRAY 16" };
        if (lcd_bars_) { lv_obj_delete(lcd_bars_); lcd_bars_ = nullptr; }

        if (lcd_page_ < 5) {
            lv_obj_set_style_bg_color(lcd_pad_, lv_color_hex(colors[lcd_page_]), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(lcd_pad_, lv_color_hex(0x000000), LV_PART_MAIN);
            lcd_bars_ = box(root_, 0, 0, SCR_W, SCR_H - 30, 0x000000, 0);
            for (int i = 0; i < 16; i++) {
                uint8_t v = (uint8_t)(i * 17);
                box(lcd_bars_, i * (SCR_W / 16), 0, SCR_W / 16, SCR_H - 30,
                    ((uint32_t)v << 16) | ((uint32_t)v << 8) | v, 0);
            }
        }
        if (lcd_txt_) {
            lv_obj_move_foreground(lcd_txt_);
            lv_obj_set_style_text_color(lcd_txt_,
                lv_color_hex(lcd_page_ == 3 ? 0x333333 : 0xFFFFFF), LV_PART_MAIN);
            lv_label_set_text_fmt(lcd_txt_, "%s   %d / 6", names[lcd_page_], lcd_page_ + 1);
        }
    }

    bool lcd_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Left:  lcd_page_ = (lcd_page_ + 5) % 6; paint_lcd(); return true;
        case tdeck::Key::Right: lcd_page_ = (lcd_page_ + 1) % 6; paint_lcd(); return true;
        case tdeck::Key::Back:  finish(Result::Skip); return true;
        case tdeck::Key::Enter: finish(Result::Pass); return true;
        case tdeck::Key::Char:
            if (ev.ch == 'y' || ev.ch == 'Y') { finish(Result::Pass); return true; }
            if (ev.ch == 'n' || ev.ch == 'N') { finish(Result::Fail); return true; }
            return true;
        default: return false;
        }
    }

    // ── 定时器 ────────────────────────────────────────────
    void start_timer(uint32_t ms, lv_timer_cb_t cb)
    {
        kill_timer();
        tmr_ = lv_timer_create(cb, ms, this);
    }
    void kill_timer()
    {
        if (tmr_) { lv_timer_delete(tmr_); tmr_ = nullptr; }
    }

    // 在建 UI 的过程中就判定失败时用:不能在 show_xxx() 里直接 finish(),
    // 那会在 lv_obj_clean 还没走完的时候把界面拆了。
    void finish_later(Result r, const char* detail)
    {
        pending_ = r;
        snprintf(pending_detail_, sizeof(pending_detail_), "%s", detail);
        start_timer(600, [](lv_timer_t* t) {
            auto* s = static_cast<SelfTestApp*>(lv_timer_get_user_data(t));
            s->kill_timer();
            s->finish(s->pending_, s->pending_detail_);
        });
    }

    static SelfTestApp* self_;
    lv_obj_t*  root_ = nullptr;
    lv_obj_t*  list_ = nullptr;
    lv_obj_t*  cards_[ST_N] = {};
    lv_timer_t* tmr_ = nullptr;
    int  view_ = -1;          // -1 = 首页,其余是站号
    int  sel_  = 0;
    Result pending_ = Result::Untested;
    char   pending_detail_[40] = {};

    // 键盘站
    bool       khit_[KSLOTS] = {};
    lv_obj_t*  kobj_[KSLOTS] = {};
    int        kcount_ = 0;
    lv_obj_t*  kecho_ = nullptr, *kprog_ = nullptr;

    // 轨迹球站
    bool       bhit_[4] = {};
    bool       bclick_ = false;
    lv_obj_t*  btgt_[4] = {};
    int        bx_[4] = {}, by_[4] = {};
    lv_obj_t*  bcur_ = nullptr, *bprog_ = nullptr;
    int        bcur_x_ = 0, bcur_y_ = 0;

    // 触摸站
    bool       thit_[4] = {};
    bool       tlong_ = false;
    lv_obj_t*  ttgt_[4] = {};
    int        tx_[4] = {}, ty_[4] = {};
    lv_obj_t*  tpad_ = nullptr, *tprog_ = nullptr;
    lv_obj_t*  tdot_[120] = {};
    int        tdots_ = 0;

    // 麦克风站
    lv_obj_t*  mvu_ = nullptr, *mvu_bg_ = nullptr, *mtxt_ = nullptr, *mstat_ = nullptr;
    lv_obj_t*  mrec_ = nullptr, *mrec_bg_ = nullptr;
    int        mhold_ms_ = 0, mpeak_ = 0;
    bool       mic_on_ = false;

    // 喇叭站
    lv_obj_t*  stxt_ = nullptr, *shint_ = nullptr;
    int        spk_step_ = 0;

    // 屏幕站
    lv_obj_t*  lcd_pad_ = nullptr, *lcd_txt_ = nullptr, *lcd_bars_ = nullptr;
    int        lcd_page_ = 0;
};

SelfTestApp* SelfTestApp::self_ = nullptr;
constexpr char SelfTestApp::KROW0[];
constexpr char SelfTestApp::KROW1[];
constexpr char SelfTestApp::KROW2[];

}  // namespace

TDECK_REGISTER_APP(SelfTestApp)
