// settings_app.cc — 系统设置
//
// 目前三块:WiFi(查看 / 扫描 / 切换)、背光、关于。
//
// WiFi 密码输入直接用 T-Deck 的物理键盘 —— 它本来就是这台机器最大的优势,
// 没必要在 320x240 上挤一个软键盘出来。

#include "app.h"
#include "net/net.h"
#include "tdeck_bsp.h"

#include <esp_mac.h>
#include <esp_system.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

// 卡片配图。tools/gen_card_art.py 生成,CMake EMBED_FILES 以二进制嵌入
// (143x94 RGB565,26884 字节),不走 C 数组。
extern "C" const uint8_t card_settings_start[] asm("_binary_card_settings_rgb565_start");
const lv_image_dsc_t kCardArt = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = 143, .h = 94, .stride = 143 * 2, .reserved_2 = 0 },
    .data_size = 143 * 94 * 2,
    .data      = card_settings_start,
};

constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_TEXT   = 0x1b2117;
constexpr uint32_t C_MUTE   = 0x6a7360;
constexpr uint32_t C_LINE   = 0xd6dbcd;
constexpr uint32_t C_CARD   = 0xffffff;

constexpr int SCR_W = 320, SCR_H = 240;
constexpr int MAX_AP = 12;

// 界面有三层:主菜单 → WiFi 列表 → 密码输入。
// 用一个枚举而不是三个 screen,是因为它们共用同一块 root,
// Back 键在层与层之间退,退到头才交还给 launcher(ADR-006 的语义)。
enum class View { Menu, WifiList, Password };

class SettingsApp : public tdeck::App {
public:
    const char* name() const override   { return "Settings"; }
    const lv_image_dsc_t* card_art() const override  { return &kCardArt; }
    const char* card_title() const override           { return "Settings"; }
    const char* icon() const override   { return LV_SYMBOL_SETTINGS; }
    uint32_t    accent() const override { return 0x4F6B3E; }
    // 密码框里要能打全部字符(ADR-006 的后路)
    bool wants_raw_keys() const override { return view_ == View::Password; }

    // 卡片是【入口】不是挂件:不显示当前网络状态。
    // 三道同心弧 —— 既像设置里的滑块刻度,又和音乐的直条、色板的色带
    // 在形状上彻底区分开。三张卡片放一起靠形状就能认,不用读字。
    void render_card(lv_obj_t* card) override
    {
        const int W = 143, H = 94;
        lv_obj_set_style_bg_color(card, lv_color_hex(0xEFF3E9), LV_PART_MAIN);

        struct { int r, w, start, len; uint32_t c; } arcs[] = {
            { 62, 7, 140, 200, 0x55853A },
            { 48, 6, 170, 150, 0x8FAE60 },
            { 34, 5, 200, 110, 0xC3D6A8 },
        };
        for (auto& a : arcs) {
            lv_obj_t* arc = lv_arc_create(card);
            lv_obj_set_size(arc, a.r * 2, a.r * 2);
            lv_obj_set_pos(arc, W / 2 - a.r, H - 18 - a.r);
            lv_arc_set_bg_angles(arc, a.start, a.start + a.len);
            lv_arc_set_value(arc, 100);
            lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
            lv_obj_set_style_arc_width(arc, a.w, LV_PART_MAIN);
            lv_obj_set_style_arc_width(arc, a.w, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(a.c), LV_PART_MAIN);
            lv_obj_set_style_arc_color(arc, lv_color_hex(a.c), LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* nm = lv_label_create(card);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_label_set_text(nm, "Settings");
        lv_obj_set_pos(nm, 12, 10);
    }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(0xF2F5EE), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        show_menu();
    }

    void on_exit() override { root_ = nullptr; list_ = nullptr; n_ap_ = 0; }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (view_) {
        case View::Menu:     return menu_input(ev);
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

    lv_obj_t* row(lv_obj_t* p, int y, int h = 40)
    {
        lv_obj_t* r = lv_obj_create(p);
        lv_obj_set_size(r, SCR_W - 24, h);
        lv_obj_set_pos(r, 12, y);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_radius(r, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(r, lv_color_hex(C_LINE), LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        return r;
    }

    void title(const char* s)
    {
        mk(root_, &lv_font_montserrat_20, C_TEXT, 14, 10, s);
    }

    void hint(const char* s)
    {
        lv_obj_t* l = mk(root_, &lv_font_montserrat_14, C_MUTE, 14, SCR_H - 22, s);
        (void)l;
    }

    // ── 主菜单 ──
    void show_menu()
    {
        view_ = View::Menu;
        lv_obj_clean(root_);
        title("Settings");

        auto st = tdeck::net_status();
        char buf[64];

        lv_obj_t* r0 = row(root_, 44);
        mk(r0, &lv_font_montserrat_16, C_TEXT, 12, 5, LV_SYMBOL_WIFI "  Wi-Fi");
        snprintf(buf, sizeof(buf), "%s", st.online ? st.ssid : "not connected");
        lv_obj_t* v0 = mk(r0, &lv_font_montserrat_14, C_MUTE, 12, 23, buf);
        lv_label_set_long_mode(v0, LV_LABEL_LONG_DOT);
        lv_obj_set_width(v0, 270);

        lv_obj_t* r1 = row(root_, 90);
        mk(r1, &lv_font_montserrat_16, C_TEXT, 12, 5, LV_SYMBOL_EYE_OPEN "  Backlight");
        snprintf(buf, sizeof(buf), "%d%%   (left/right to adjust)", tdeck_backlight_get());
        bl_val_ = mk(r1, &lv_font_montserrat_14, C_MUTE, 12, 23, buf);

        lv_obj_t* r2 = row(root_, 136, 56);
        mk(r2, &lv_font_montserrat_16, C_TEXT, 12, 5, LV_SYMBOL_LIST "  About");
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(buf, sizeof(buf), "T-Deck OS   %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mk(r2, &lv_font_montserrat_14, C_MUTE, 12, 23, buf);
        snprintf(buf, sizeof(buf), "free %u KB", (unsigned)(esp_get_free_heap_size() / 1024));
        mk(r2, &lv_font_montserrat_14, C_MUTE, 12, 38, buf);

        rows_[0] = r0; rows_[1] = r1; rows_[2] = r2;
        n_rows_ = 3;
        paint_rows();
        hint("y: open   n/b: back   q: quit");
    }

    void paint_rows()
    {
        for (int i = 0; i < n_rows_; i++) {
            bool on = (i == sel_);
            lv_obj_set_style_border_color(rows_[i], lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(rows_[i], on ? 2 : 1, LV_PART_MAIN);
        }
    }

    bool menu_input(const tdeck::InputEvent& ev)
    {
        switch (ev.key) {
        case tdeck::Key::Up:    if (sel_ > 0) { sel_--; paint_rows(); } return true;
        case tdeck::Key::Down:  if (sel_ < n_rows_ - 1) { sel_++; paint_rows(); } return true;
        case tdeck::Key::Left:
        case tdeck::Key::Right:
            if (sel_ == 1) {   // 背光行:左右直接调,不用进二级页
                int d = (ev.key == tdeck::Key::Right) ? 5 : -5;
                int v = (int)tdeck_backlight_get() + d;
                if (v < 5)   v = 5;
                if (v > 100) v = 100;
                tdeck_backlight_set((uint8_t)v);
                char b[64]; snprintf(b, sizeof(b), "%d%%   (left/right to adjust)", v);
                lv_label_set_text(bl_val_, b);
                return true;
            }
            return false;   // 其它行让 launcher 翻页
        case tdeck::Key::Enter:
            if (sel_ == 0) { show_wifi_list(); }
            return true;
        default: return false;
        }
    }

    // ── WiFi 列表 ──
    void show_wifi_list()
    {
        view_ = View::WifiList;
        lv_obj_clean(root_);
        title("Wi-Fi");
        mk(root_, &lv_font_montserrat_14, C_MUTE, 14, 36, "scanning...");
        lv_refr_now(nullptr);   // 扫描是阻塞的,先把"扫描中"刷出去

        n_ap_ = tdeck::net_scan(aps_, MAX_AP);
        sel_ = 0;

        lv_obj_clean(root_);
        title("Wi-Fi");
        if (n_ap_ == 0) {
            mk(root_, &lv_font_montserrat_16, C_MUTE, 14, 40, "no networks found");
            hint("n/b: back");
            return;
        }
        list_ = lv_obj_create(root_);
        lv_obj_set_size(list_, SCR_W - 24, 176);
        lv_obj_set_pos(list_, 12, 38);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < n_ap_; i++) {
            lv_obj_t* r = lv_obj_create(list_);
            lv_obj_set_size(r, SCR_W - 28, 30);
            lv_obj_set_pos(r, 0, i * 34);
            lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD), LV_PART_MAIN);
            lv_obj_set_style_radius(r, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

            char b[64];
            snprintf(b, sizeof(b), "%s%s", aps_[i].ssid, aps_[i].secure ? "  " LV_SYMBOL_CLOSE : "");
            lv_obj_t* l = mk(r, &lv_font_montserrat_16, C_TEXT, 10, 4, b);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l, 210);
            snprintf(b, sizeof(b), "%d", aps_[i].rssi);
            mk(r, &lv_font_montserrat_14, C_MUTE, SCR_W - 70, 6, b);
            ap_rows_[i] = r;
        }
        paint_ap();
        hint("y: connect   n/b: back");
    }

    void paint_ap()
    {
        for (int i = 0; i < n_ap_; i++) {
            bool on = (i == sel_);
            lv_obj_set_style_border_color(ap_rows_[i], lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(ap_rows_[i], on ? 2 : 1, LV_PART_MAIN);
        }
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
        case tdeck::Key::Back: show_menu(); return true;   // 退回上一层,不交给 launcher
        default: return false;
        }
    }

    // ── 密码输入 ──
    void show_password()
    {
        view_ = View::Password;
        pass_len_ = 0; pass_[0] = 0;
        lv_obj_clean(root_);
        title("Password");
        mk(root_, &lv_font_montserrat_16, C_MUTE, 14, 42, aps_[sel_].ssid);

        lv_obj_t* box = row(root_, 76, 44);
        pass_lbl_ = mk(box, &lv_font_montserrat_20, C_TEXT, 12, 10, "");

        mk(root_, &lv_font_montserrat_14, C_MUTE, 14, 132,
           "type on the keyboard, then press Enter");
        hint("Enter: connect   ESC: back");
    }

    bool pass_input(const tdeck::InputEvent& ev)
    {
        if (ev.key == tdeck::Key::Back) { show_wifi_list(); return true; }
        if (ev.key == tdeck::Key::Enter) {
            tdeck::net_set_credentials(aps_[sel_].ssid, pass_);
            lv_obj_clean(root_);
            title("Wi-Fi");
            mk(root_, &lv_font_montserrat_16, C_TEXT, 14, 50, "connecting...");
            mk(root_, &lv_font_montserrat_14, C_MUTE, 14, 78, aps_[sel_].ssid);
            hint("n/b: back");
            view_ = View::Menu;   // 连接是异步的,回到菜单,状态自己会更新
            sel_ = 0;
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
    lv_obj_t*  list_ = nullptr, *bl_val_ = nullptr, *pass_lbl_ = nullptr;
    tdeck::ApInfo aps_[MAX_AP];
    char       pass_[65] = {};
    int        pass_len_ = 0;
    int        sel_ = 0, n_rows_ = 0, n_ap_ = 0;
    View       view_ = View::Menu;
};

}  // namespace

TDECK_REGISTER_APP(SettingsApp)
