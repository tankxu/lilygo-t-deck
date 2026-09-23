// launcher.cc — 应用宿主 + 桌面
//
// 职责分两块:
//   Host      —— 订阅 BSP 输入,归一化后路由给当前 app;管 app 的进入/退出
//   Launcher  —— 应用列表 UI
//
// 全局约定:【任何 app 里按 q 退出回桌面】。q 在 Host 这一层就被截下来,
// 不会下发给 app —— 否则某个 app 忘了处理或把它当普通字符吃掉,人就被困在里面了。

#include "app.h"
#include "tdeck_bsp.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <lvgl.h>

namespace tdeck {
namespace {

constexpr uint32_t C_BG     = 0xf6f8fa;
constexpr uint32_t C_CARD   = 0xffffff;
constexpr uint32_t C_SEL    = 0xddf4ff;
constexpr uint32_t C_ACCENT = 0x0969da;
constexpr uint32_t C_TEXT   = 0x1f2328;
constexpr uint32_t C_MUTE   = 0x57606a;
constexpr uint32_t C_LINE   = 0xd1d9e0;

constexpr int MAX_ITEMS = 12;
constexpr int ROW_H     = 34;
constexpr int LIST_Y    = 44;

const char* TAG = "launcher";

class Host {
public:
    static Host& instance() { static Host h; return h; }

    void begin()
    {
        count_ = (int)AppRegistry::instance().apps().size();
        if (count_ > MAX_ITEMS) count_ = MAX_ITEMS;
        build_launcher();
        show_launcher();
        tdeck_input_subscribe(&Host::bsp_cb, this);
        ESP_LOGI(TAG, "桌面就绪,%d 个应用", count_);
    }

private:
    // ── UI ──
    void build_launcher()
    {
        launcher_scr_ = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(launcher_scr_, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(launcher_scr_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(launcher_scr_);
        lv_label_set_text(title, "T-Deck OS");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_pos(title, 10, 10);

        bat_ = lv_label_create(launcher_scr_);
        lv_obj_set_style_text_color(bat_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_align(bat_, LV_ALIGN_TOP_RIGHT, -10, 14);
        update_battery();

        auto& apps = AppRegistry::instance().apps();
        for (int i = 0; i < count_; i++) {
            lv_obj_t* row = lv_obj_create(launcher_scr_);
            lv_obj_set_size(row, 300, ROW_H - 4);
            lv_obj_set_pos(row, 10, LIST_Y + i * ROW_H);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* lbl = lv_label_create(row);
            lv_label_set_text(lbl, apps[i]->name());
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

            items_[i] = row;
        }

        lv_obj_t* hint = lv_label_create(launcher_scr_);
        lv_label_set_text(hint, "ball/click move+open   y=ok  n,b=back  q=quit");
        lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 10, -8);

        paint_selection();
    }

    void paint_selection()
    {
        for (int i = 0; i < count_; i++) {
            bool on = (i == sel_);
            lv_obj_set_style_bg_color(items_[i], lv_color_hex(on ? C_SEL : C_CARD), LV_PART_MAIN);
            lv_obj_set_style_border_color(items_[i],
                lv_color_hex(on ? C_ACCENT : C_LINE), LV_PART_MAIN);
            lv_obj_set_style_border_width(items_[i], on ? 2 : 1, LV_PART_MAIN);
        }
    }

    void update_battery()
    {
        int mv = tdeck_battery_mv();
        if (mv > 0) lv_label_set_text_fmt(bat_, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
        else        lv_label_set_text(bat_, "-- V");
    }

    void show_launcher()
    {
        update_battery();
        lv_screen_load(launcher_scr_);
    }

    // ── 生命周期 ──
    void open(int i)
    {
        auto& apps = AppRegistry::instance().apps();
        if (i < 0 || i >= (int)apps.size()) return;

        current_  = apps[i];
        app_scr_  = lv_obj_create(nullptr);
        current_->on_enter(app_scr_);
        lv_screen_load(app_scr_);
        ESP_LOGI(TAG, "进入 %s", current_->name());
    }

    void back()
    {
        if (!current_) return;
        ESP_LOGI(TAG, "退出 %s", current_->name());

        // 先切回桌面再销毁 app 的 screen —— 反过来会短暂地没有活动 screen
        show_launcher();
        current_->on_exit();
        if (app_scr_) { lv_obj_delete(app_scr_); app_scr_ = nullptr; }
        current_ = nullptr;
    }

    // ── 输入路由 ──
    static void bsp_cb(const tdeck_input_event_t* ev, void* user)
    {
        static_cast<Host*>(user)->handle(*ev);
    }

    void handle(const tdeck_input_event_t& ev)
    {
        // 回调跑在 BSP 的输入任务里,碰 LVGL 必须先拿锁
        lvgl_port_lock(0);

        InputEvent ie{};
        if (ev.source == TDECK_INPUT_TRACKBALL) {
            switch (ev.code) {
            case TDECK_TB_UP:    ie.key = Key::Up;    break;
            case TDECK_TB_DOWN:  ie.key = Key::Down;  break;
            case TDECK_TB_LEFT:  ie.key = Key::Left;  break;
            case TDECK_TB_RIGHT: ie.key = Key::Right; break;
            case TDECK_TB_CLICK: ie.key = Key::Enter; break;
            default: break;
            }
        } else if (ev.source == TDECK_INPUT_KEYBOARD) {
            // ── OS 级键位约定(ADR-006)────────────────────────────────
            // 统一在这一处定义,app 不各自解释按键 —— 否则每个 app 的
            // 「确定 / 返回」都不一样,用户得逐个记。
            //
            //   y · Enter · 轨迹球中键   → Enter   确定、打开
            //   n · b · ESC              → Back    返回上一步、取消
            //   q                        → 硬退出,直接回桌面
            //
            // Back 和 q 看着像,语义是有意分开的:
            //   Back 先下发给 app。app 可以拿它关掉自己的子界面(对话框、
            //        二级菜单),只有 app 不处理时才退回桌面 —— 这才叫
            //        「返回上一步」。
            //   q    在这里直接截获,不下发。它是逃生口:无论 app 处于什么
            //        状态、有没有 bug,按 q 一定能出来。
            switch (ev.code) {
            case 'q': case 'Q':
                if (current_) { back(); lvgl_port_unlock(); return; }
                break;
            case 'y': case 'Y':
            case 13: case 10:
                ie.key = Key::Enter; break;
            case 'n': case 'N':
            case 'b': case 'B':
            case 27:
                ie.key = Key::Back;  break;
            default:
                ie.key = Key::Char; ie.ch = (char)ev.code; break;
            }
        }

        if (current_) {
            if (!current_->on_input(ie) && ie.key == Key::Back) back();
        } else {
            switch (ie.key) {
            case Key::Up:    sel_ = (sel_ - 1 + count_) % count_; paint_selection(); break;
            case Key::Down:  sel_ = (sel_ + 1) % count_;          paint_selection(); break;
            case Key::Enter: open(sel_);                           break;
            default: break;
            }
        }

        lvgl_port_unlock();
    }

    App*       current_      = nullptr;
    lv_obj_t*  app_scr_      = nullptr;
    lv_obj_t*  launcher_scr_ = nullptr;
    lv_obj_t*  items_[MAX_ITEMS] = {};
    lv_obj_t*  bat_          = nullptr;
    int        sel_   = 0;
    int        count_ = 0;
};

}  // namespace

void launcher_begin() { Host::instance().begin(); }

}  // namespace tdeck
