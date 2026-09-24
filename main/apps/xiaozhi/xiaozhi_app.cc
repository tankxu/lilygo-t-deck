// xiaozhi_app.cc — 小智
//
// 入口有两个,都不是应用页上的卡片(ADR-004:小智是常驻角色不是普通应用):
//   · 首屏左上角的头像
//   · 键盘数字键 0(任何界面下都生效,由 Host 全局截获)
//
// 这一层只管 UI 和状态机接线。音频链路和协议栈在 components/xiaozhi 里,
// 通过 xiaozhi_core.h 的几个函数对接 —— 界面不该知道 Opus 和 WebSocket 的存在。

#include "app.h"
#include "launcher/avatar.h"
#include "tdeck_bsp.h"

#include <esp_log.h>
#include <lvgl.h>
#include <string.h>

namespace {

// 背景跟随整机的浅色系。只有【脸本身】是深色 —— 亮绿的眼睛需要深底才发得出光,
// 那是一个 150px 的圆,不构成大面积深色,不会把背光漏光衬出来。
constexpr uint32_t C_BG     = 0xFFFFFF;
constexpr uint32_t C_TEXT   = 0x1B2117;
constexpr uint32_t C_MUTE   = 0x8A9480;
constexpr uint32_t C_ACCENT = 0x55853A;
constexpr int SCR_W = 320, SCR_H = 240;

class XiaozhiApp : public tdeck::App {
public:
    const char* name() const override   { return "XIAOZHI"; }
    const char* icon() const override   { return LV_SYMBOL_VOLUME_MAX; }
    uint32_t    accent() const override { return C_ACCENT; }

    // 小智不出现在应用页 —— 它的入口是首屏头像和数字键 0。
    // launcher 靠这个标志把它从卡片网格里排除。
    bool hidden_from_grid() const override { return true; }

    // 常驻后台:切走只收起 UI,连接和音频线程继续活着(ADR-004)
    bool wants_background() const override { return true; }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        avatar_.create(root_, SCR_W / 2, 96, 150);

        status_ = lv_label_create(root_);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(status_, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_width(status_, SCR_W);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(status_, 0, 182);
        lv_label_set_text(status_, "idle");

        hint_ = lv_label_create(root_);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_width(hint_, SCR_W);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(hint_, 0, 210);
        lv_label_set_text(hint_, "y / hold ball: talk     q: back");
    }

    void on_exit() override
    {
        avatar_.destroy();
        root_ = status_ = hint_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (ev.key == tdeck::Key::Enter) { toggle_talk(); return true; }
        return false;
    }

    // 全局语音键(轨迹球中键长按)按下时由 Host 调进来
    void on_voice_key()
    {
        toggle_talk();
    }

private:
    void toggle_talk()
    {
        talking_ = !talking_;
        set_state(talking_ ? tdeck::Avatar::State::Listening
                           : tdeck::Avatar::State::Idle);
    }

    void set_state(tdeck::Avatar::State s)
    {
        avatar_.set_state(s);
        if (!status_) return;
        switch (s) {
        case tdeck::Avatar::State::Listening: lv_label_set_text(status_, "listening"); break;
        case tdeck::Avatar::State::Speaking:  lv_label_set_text(status_, "speaking");  break;
        default:                              lv_label_set_text(status_, "idle");      break;
        }
    }

    lv_obj_t*     root_ = nullptr, *status_ = nullptr, *hint_ = nullptr;
    tdeck::Avatar avatar_;
    bool          talking_ = false;
};

}  // namespace

TDECK_REGISTER_APP(XiaozhiApp)
