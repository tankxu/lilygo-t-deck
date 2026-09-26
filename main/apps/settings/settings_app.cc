// settings_app.cc — 系统设置
//
// 四层:主菜单 → 关于本机 / WiFi 列表 → 密码输入。
// 用一个枚举而不是四个 screen,是因为它们共用同一块 root,
// Back 键在层与层之间退,退到头才交还给 launcher(ADR-006 的语义)。
//
// WiFi 密码输入直接用 T-Deck 的物理键盘 —— 它本来就是这台机器最大的优势,
// 没必要在 320x240 上挤一个软键盘出来。
//
// 视觉上和音乐、自检是一套:中文、圆角白卡、左侧彩色图标方块。
// ⚠️ 界面里【不写快捷键】—— 全部进 shortcuts(),由 launcher 的 s 键统一展示。

#include "app.h"
#include "net/net.h"
#include "tdeck_bsp.h"
#include "ui/fonts.h"

#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_TEXT   = 0x1b2117;
constexpr uint32_t C_MUTE   = 0x8A9480;
constexpr uint32_t C_CARD   = 0xFFFFFF;
constexpr uint32_t C_SEL_BG = 0xEDF3E4;   // 选中行的浅绿底
constexpr uint32_t C_TRACK  = 0xE2E8D8;   // 亮度条的槽

constexpr int SCR_W = 320, SCR_H = 240;
constexpr int MAX_AP = 12;

constexpr int SIDE   = 12;
constexpr int ROW_H  = 54;
constexpr int ROW_GAP = 8;
constexpr int ROW_R  = 14;
constexpr int CHIP   = 34;          // 左侧图标方块

// 字体:能用中文就用中文,建不起来时回落到 Montserrat(只有 ASCII)。
const lv_font_t* F20() { auto* f = tdeck::font_cjk();       return f ? f : &lv_font_montserrat_20; }
const lv_font_t* F16() { auto* f = tdeck::font_cjk_small(); return f ? f : &lv_font_montserrat_16; }
const lv_font_t* FICON() { return &lv_font_montserrat_16; }   // LV_SYMBOL_* 在这套字库里

enum class View { Menu, About, WifiList, Password };

class SettingsApp : public tdeck::App {
public:
    const char* name() const override   { return "Settings"; }
    uint32_t card_color() const override              { return 0x2F6690; }
    const char* card_title() const override           { return "Settings"; }
    const char* icon() const override   { return LV_SYMBOL_SETTINGS; }
    uint32_t    accent() const override { return 0x4F6B3E; }
    // 密码框里要能打全部字符(ADR-006 的后路)
    bool wants_raw_keys() const override { return view_ == View::Password; }

    int shortcuts(const tdeck::Shortcut** out) const override
    {
        static const tdeck::Shortcut menu[] = {
            { "ball U/D",  "move between rows" },
            { "ball L/R",  "brightness (on that row)" },
            { "y / click", "open" },
        };
        static const tdeck::Shortcut list[] = {
            { "ball U/D",  "pick a network" },
            { "y / click", "connect" },
            { "n / b",     "back" },
        };
        static const tdeck::Shortcut pass[] = {
            { "type",      "the password" },
            { "enter",     "connect" },
            { "esc",       "back" },
        };
        static const tdeck::Shortcut about[] = {
            { "n / b",     "back" },
        };
        switch (view_) {
        case View::Menu:     *out = menu;  return 3;
        case View::WifiList: *out = list;  return 3;
        case View::Password: *out = pass;  return 3;
        case View::About:    *out = about; return 1;
        }
        return 0;
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xF4F6F0), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        show_menu();
    }

    void on_exit() override { root_ = nullptr; list_ = nullptr; n_ap_ = 0; }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (view_) {
        case View::Menu:     return menu_input(ev);
        case View::About:    return about_input(ev);
        case View::WifiList: return list_input(ev);
        case View::Password: return pass_input(ev);
        }
        return false;
    }

private:
    // ── 小工具 ──
    static lv_obj_t* mk(lv_obj_t* p, const lv_font_t* f, uint32_t c, int x, int y, const char* s)
    {
        lv_obj_t* l = lv_label_create(p);
        lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
        lv_label_set_text(l, s);
        lv_obj_set_pos(l, x, y);
        return l;
    }

    // 一块素的圆角面板。所有卡片、方块、进度槽都从它长出来 ——
    // lv_obj_create 默认带边框、内边距和滚动,每处都重设一遍太啰嗦。
    static lv_obj_t* panel(lv_obj_t* p, int x, int y, int w, int h, uint32_t bg, int r)
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
        // ⚠️ lv_obj_create 建出来的对象【默认可点】。这些都是装饰,
        // 不摘掉就会把点击从父行那儿抢走。
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        return o;
    }

    // 行左边那个彩色图标方块。颜色按功能分,比一排同色图标好认。
    static void chip(lv_obj_t* r, uint32_t color, const char* sym)
    {
        lv_obj_t* c = panel(r, 10, (ROW_H - CHIP) / 2, CHIP, CHIP, color, 10);
        lv_obj_t* l = mk(c, FICON(), 0xFFFFFF, 0, 0, sym);
        lv_obj_center(l);
    }

    // 可点的行。panel() 默认把 CLICKABLE 摘了(那是给装饰子对象用的),
    // 行要自己加回来 —— 这台机器有触摸屏,列表点不动不像话。
    // idx 存进 user_data,回调里直接拿,不用为每行建一个 lambda 闭包。
    lv_obj_t* make_row(int y, int idx, lv_event_cb_t cb, int h = ROW_H)
    {
        lv_obj_t* r = panel(root_, SIDE, y, SCR_W - SIDE * 2, h, C_CARD, ROW_R);
        lv_obj_set_user_data(r, (void*)(intptr_t)idx);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, this);
        return r;
    }

    // 点菜单行 = 选中它并打开(亮度行没有二级页,只选中)。
    static void menu_click(lv_event_t* e)
    {
        auto* s = static_cast<SettingsApp*>(lv_event_get_user_data(e));
        int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
        s->sel_ = i;
        s->paint_rows();
        if (i == 0)      s->show_wifi_list();
        else if (i == 2) s->show_about();
    }

    static void ap_click(lv_event_t* e)
    {
        auto* s = static_cast<SettingsApp*>(lv_event_get_user_data(e));
        s->sel_ = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
        s->paint_ap();
        s->show_password();
    }

    void header(const char* s)
    {
        mk(root_, F20(), C_TEXT, SIDE + 2, 8, s);
    }

    // 选中态:浅绿底 + 2px accent 描边。
    //
    // ⚠️ 边框宽度【一直是 2】,切换的是透明度。
    // LVGL 里子对象的坐标是相对【内容区】算的,而内容区 = 外框 - 边框宽 - 内边距。
    // 所以 0 → 2 的边框变化会把整行的图标和文字一起往右下推,
    // 选中时肉眼可见地跳一下(实测图标方块 x 从 14 变成 15)。
    // 透明度不参与内容区计算,改它就不会动布局。
    // 音乐列表那圈选中环踩的是同一个坑,那边是另建一个同心对象解决的;
    // 这里整行本来就要换底色,恒宽 + 调透明度更省一个对象。
    static void mark(lv_obj_t* o, bool on)
    {
        lv_obj_set_style_bg_color(o, lv_color_hex(on ? C_SEL_BG : C_CARD), LV_PART_MAIN);
        lv_obj_set_style_border_width(o, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(o, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_opa(o, on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    }

    // ── 主菜单 ──
    void show_menu()
    {
        view_ = View::Menu;
        lv_obj_clean(root_);
        bl_bar_ = bl_val_ = nullptr;
        header("设置");

        auto st = tdeck::net_status();
        char buf[64];
        int y = 44;

        // 无线网络
        lv_obj_t* r0 = make_row(y, 0, menu_click);
        chip(r0, 0x3B82C4, LV_SYMBOL_WIFI);
        mk(r0, F16(), C_TEXT, 56, 8, "无线网络");
        snprintf(buf, sizeof(buf), "%s", st.online ? st.ssid : "未连接");
        lv_obj_t* v0 = mk(r0, F16(), C_MUTE, 56, 29, buf);
        lv_label_set_long_mode(v0, LV_LABEL_LONG_DOT);
        lv_obj_set_width(v0, SCR_W - SIDE * 2 - 56 - 28);
        mk(r0, FICON(), C_MUTE, SCR_W - SIDE * 2 - 22, ROW_H / 2 - 9, LV_SYMBOL_RIGHT);
        y += ROW_H + ROW_GAP;

        // 屏幕亮度 —— 直接给一条进度槽。百分比数字也留着,
        // 因为槽只能看出"大概多亮",调到底/调到顶时数字才说得清。
        lv_obj_t* r1 = make_row(y, 1, menu_click);
        chip(r1, 0xD79A32, LV_SYMBOL_EYE_OPEN);
        mk(r1, F16(), C_TEXT, 56, 8, "屏幕亮度");
        int bl = (int)tdeck_backlight_get();
        snprintf(buf, sizeof(buf), "%d%%", bl);
        bl_val_ = mk(r1, F16(), C_MUTE, SCR_W - SIDE * 2 - 48, 8, buf);
        const int TRK_X = 56, TRK_W = SCR_W - SIDE * 2 - TRK_X - 14;
        panel(r1, TRK_X, 34, TRK_W, 6, C_TRACK, 3);
        bl_bar_ = panel(r1, TRK_X, 34, TRK_W * bl / 100, 6, C_ACCENT, 3);
        bl_trk_w_ = TRK_W;
        y += ROW_H + ROW_GAP;

        // 关于本机 —— 详情挪进二级页。首页塞 MAC 和堆大小,
        // 又长又没人天天看,把这一行撑得比别的行都高。
        lv_obj_t* r2 = make_row(y, 2, menu_click);
        chip(r2, 0x7A8471, LV_SYMBOL_LIST);
        mk(r2, F16(), C_TEXT, 56, 8, "关于本机");
        const esp_app_desc_t* app = esp_app_get_description();
        snprintf(buf, sizeof(buf), "T-Deck OS %s", app->version);
        mk(r2, F16(), C_MUTE, 56, 29, buf);
        mk(r2, FICON(), C_MUTE, SCR_W - SIDE * 2 - 22, ROW_H / 2 - 9, LV_SYMBOL_RIGHT);

        rows_[0] = r0; rows_[1] = r1; rows_[2] = r2;
        n_rows_ = 3;
        if (sel_ >= n_rows_) sel_ = 0;
        paint_rows();
    }

    void paint_rows()
    {
        for (int i = 0; i < n_rows_; i++) mark(rows_[i], i == sel_);
    }

    bool menu_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Up:    if (sel_ > 0) { sel_--; paint_rows(); } return true;
        case tdeck::Key::Down:  if (sel_ < n_rows_ - 1) { sel_++; paint_rows(); } return true;
        case tdeck::Key::Left:
        case tdeck::Key::Right:
            if (sel_ == 1) {   // 亮度行:左右直接调,不用进二级页
                int d = (ev.key == tdeck::Key::Right) ? 5 : -5;
                int v = (int)tdeck_backlight_get() + d;
                if (v < 5)   v = 5;
                if (v > 100) v = 100;
                tdeck_backlight_set((uint8_t)v);
                char b[16]; snprintf(b, sizeof(b), "%d%%", v);
                lv_label_set_text(bl_val_, b);
                lv_obj_set_width(bl_bar_, bl_trk_w_ * v / 100);
                return true;
            }
            return false;   // 其它行让 launcher 翻页
        case tdeck::Key::Enter:
            if (sel_ == 0)      show_wifi_list();
            else if (sel_ == 2) show_about();
            return true;
        default: return false;
        }
    }

    // ── 关于本机 ──
    void show_about()
    {
        view_ = View::About;
        lv_obj_clean(root_);
        header("关于本机");

        const esp_app_desc_t* app = esp_app_get_description();
        const esp_partition_t* run = esp_ota_get_running_partition();
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        int64_t up = esp_timer_get_time() / 1000000;

        char v[6][64];
        snprintf(v[0], 64, "%s", app->version);
        snprintf(v[1], 64, "%s", app->idf_ver);
        snprintf(v[2], 64, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(v[3], 64, "%s", run ? run->label : "?");
        snprintf(v[4], 64, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024));
        if (up >= 3600)   snprintf(v[5], 64, "%d 小时 %d 分", (int)(up / 3600), (int)(up % 3600 / 60));
        else if (up >= 60) snprintf(v[5], 64, "%d 分 %d 秒", (int)(up / 60), (int)(up % 60));
        else               snprintf(v[5], 64, "%d 秒", (int)up);
        static const char* K[6] = { "系统版本", "ESP-IDF", "MAC 地址",
                                    "固件分区", "可用内存", "运行时长" };

        // 六行挤一块白卡里。分隔线用极淡的灰,比给每行一张卡片安静得多 ——
        // 这是只读信息,不需要每条都长得像个可点的按钮。
        const int RH = 28, PAD = 10;
        lv_obj_t* card = panel(root_, SIDE, 42, SCR_W - SIDE * 2, PAD * 2 + RH * 6, C_CARD, ROW_R);
        for (int i = 0; i < 6; i++) {
            int ry = PAD + i * RH;
            mk(card, F16(), C_MUTE, 14, ry + 4, K[i]);
            lv_obj_t* l = mk(card, F16(), C_TEXT, 110, ry + 4, v[i]);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l, SCR_W - SIDE * 2 - 110 - 14);
            if (i) panel(card, 14, ry - 1, SCR_W - SIDE * 2 - 28, 1, 0xEFF2EA, 0);
        }
    }

    bool about_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { sel_ = 2; show_menu(); return true; }
        return false;
    }

    // ── WiFi 列表 ──
    void show_wifi_list()
    {
        view_ = View::WifiList;
        lv_obj_clean(root_);
        header("无线网络");
        mk(root_, F16(), C_MUTE, SIDE + 2, 46, "正在扫描…");
        lv_refr_now(nullptr);   // 扫描是阻塞的,先把"扫描中"刷出去

        n_ap_ = tdeck::net_scan(aps_, MAX_AP);
        sel_ = 0;

        lv_obj_clean(root_);
        header("无线网络");
        if (n_ap_ == 0) {
            mk(root_, F16(), C_MUTE, SIDE + 2, 46, "没有找到网络");
            return;
        }
        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W, SCR_H - 42);
        lv_obj_set_pos(list_, 0, 42);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        const int AH = 40, AG = 6;
        for (int i = 0; i < n_ap_; i++) {
            lv_obj_t* r = panel(list_, SIDE, i * (AH + AG), SCR_W - SIDE * 2, AH, C_CARD, 12);
            lv_obj_set_user_data(r, (void*)(intptr_t)i);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);   // 竖滑手势要能穿过去
            lv_obj_add_event_cb(r, ap_click, LV_EVENT_CLICKED, this);
            char b[64];
            snprintf(b, sizeof(b), "%s", aps_[i].ssid);
            lv_obj_t* l = mk(r, F16(), C_TEXT, 12, (AH - 20) / 2, b);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l, 190);

            // 开放网络明着标出来。原来是给【加密】的网络打一个 LV_SYMBOL_CLOSE
            // (其实是个叉),既不像锁,也让"有标记"读起来像"连不上"。
            // 值得提醒的本来就是开放网络。
            if (!aps_[i].secure)
                mk(r, F16(), 0xC2410C, 214, (AH - 20) / 2, "开放");

            snprintf(b, sizeof(b), "%d", aps_[i].rssi);
            mk(r, F16(), C_MUTE, SCR_W - SIDE * 2 - 46, (AH - 20) / 2, b);
            ap_rows_[i] = r;
        }
        paint_ap();
    }

    void paint_ap()
    {
        for (int i = 0; i < n_ap_; i++) mark(ap_rows_[i], i == sel_);
        if (n_ap_ > 0) lv_obj_scroll_to_view(ap_rows_[sel_], LV_ANIM_ON);
    }

    bool list_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Up:   if (sel_ > 0) { sel_--; paint_ap(); } return true;
        case tdeck::Key::Down: if (sel_ < n_ap_ - 1) { sel_++; paint_ap(); } return true;
        case tdeck::Key::Enter:
            if (n_ap_ > 0) show_password();
            return true;
        case tdeck::Key::Back: sel_ = 0; show_menu(); return true;   // 退回上一层
        default: return false;
        }
    }

    // ── 密码输入 ──
    void show_password()
    {
        view_ = View::Password;
        pass_len_ = 0; pass_[0] = 0;
        lv_obj_clean(root_);
        header("输入密码");
        mk(root_, F16(), C_MUTE, SIDE + 2, 42, aps_[sel_].ssid);

        lv_obj_t* box = panel(root_, SIDE, 70, SCR_W - SIDE * 2, 48, C_CARD, ROW_R);
        lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        pass_lbl_ = mk(box, F20(), C_TEXT, 14, 12, "");
    }

    bool pass_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { show_wifi_list(); return true; }
        if (ev.key == tdeck::Key::Enter) {
            tdeck::net_set_credentials(aps_[sel_].ssid, pass_);
            lv_obj_clean(root_);
            header("无线网络");
            mk(root_, F16(), C_TEXT, SIDE + 2, 50, "正在连接…");
            mk(root_, F16(), C_MUTE, SIDE + 2, 76, aps_[sel_].ssid);
            view_ = View::Menu;   // 连接是异步的,回到菜单,状态自己会更新
            sel_ = 0;
            n_rows_ = 0;          // 这一屏不是菜单的那三行,别让方向键去动它们
            return true;
        }
        if (ev.key == tdeck::Key::Char) {
            // 8 是退格。T-Deck 键盘的删除键送的就是这个。
            if (ev.ch == 8) { if (pass_len_) pass_[--pass_len_] = 0; }
            else if (ev.ch >= 32 && ev.ch < 127 && pass_len_ < (int)sizeof(pass_) - 1) {
                pass_[pass_len_++] = ev.ch; pass_[pass_len_] = 0;
            }
            // 显示成圆点,但留最后一位可见 —— 手机上的通行做法,
            // 在物理键盘打错时能立刻发现,又不至于整串暴露
            char shown[68];
            int i = 0;
            for (; i < pass_len_ - 1 && i < 60; i++) shown[i] = '*';
            if (pass_len_ > 0) shown[i++] = pass_[pass_len_ - 1];
            shown[i] = 0;
            lv_label_set_text(pass_lbl_, shown);
            return true;
        }
        return false;
    }

    lv_obj_t*  root_ = nullptr;
    lv_obj_t*  rows_[4] = {};
    lv_obj_t*  ap_rows_[MAX_AP] = {};
    lv_obj_t*  list_ = nullptr, *bl_val_ = nullptr, *bl_bar_ = nullptr, *pass_lbl_ = nullptr;
    int        bl_trk_w_ = 1;
    tdeck::ApInfo aps_[MAX_AP];
    char       pass_[65] = {};
    int        pass_len_ = 0;
    int        sel_ = 0, n_rows_ = 0, n_ap_ = 0;
    View       view_ = View::Menu;
};

}  // namespace

TDECK_REGISTER_APP(SettingsApp)
