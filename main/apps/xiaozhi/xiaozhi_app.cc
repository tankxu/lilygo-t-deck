// xiaozhi_app.cc — 小智
//
// 入口有三个,都不是应用页上的卡片(ADR-004:小智是常驻角色不是普通应用):
//   · 首屏左上角的头像
//   · 键盘数字键 0(任何界面下都生效,由 Host 全局截获)
//   · 轨迹球中键长按(全局语音键,任何 app 里都能唤起 —— 见文件末尾的注册)
//
// 这一层只管 UI 和状态机接线。音频链路和协议栈在 components/xiaozhi 里,
// 通过 xiaozhi_core.h 的几个函数对接 —— 界面不该知道 Opus 和 WebSocket 的存在。
//
// 生命周期是【和别的 app 反着来】的:on_exit 只拆 UI,内核不停
// (连接、音频线程继续活着)。所以这个类里所有 lv_obj_t* 在 on_exit 之后
// 都必须置空,而回调里每一处都要判空 —— 小智在后台说话时,
// 用户完全可能正在音乐 app 里,这时回调照样会来。

#include "app.h"
#include "launcher/avatar.h"
#include "ui/fonts.h"
#include "xiaozhi_core.h"
#include "tdeck_bsp.h"

#include <esp_log.h>
#include <lvgl.h>
#include <string>
#include <string.h>

namespace {

static const char* TAG = "xiaozhi_app";

// 背景跟随整机的浅色系。只有【脸本身】是深色 —— 亮绿的眼睛需要深底才发得出光,
// 那是一个 130px 的圆,不构成大面积深色,不会把背光漏光衬出来。
constexpr uint32_t C_BG     = 0xFFFFFF;
constexpr uint32_t C_TEXT   = 0x1B2117;
constexpr uint32_t C_MUTE   = 0x8A9480;
constexpr uint32_t C_ACCENT = 0x55853A;
constexpr uint32_t C_CARD   = 0xF2F5EE;   // 极浅的军绿灰,和主题色同一个色相
constexpr int SCR_W = 320, SCR_H = 240;

// 中文字体没建起来时回落到 Montserrat(只有 ASCII,中文会变豆腐块,
// 但至少不崩)。字体是 OS 管的,这里只借用。
static const lv_font_t* cjk()
{
    const lv_font_t* f = tdeck::font_cjk();
    return f ? f : &lv_font_montserrat_20;
}

class XiaozhiApp : public tdeck::App {
public:
    XiaozhiApp()
    {
        // 全局语音键(ADR-004:轨迹球中键长按 600ms)。在构造函数里注册,
        // 也就是【开机就生效】,不需要先进过一次小智 —— 这正是"常驻"的含义。
        // 这个回调跑在 BSP 的输入任务里,不是 LVGL 任务,所以里面只能调
        // xz:: 这类线程安全的函数,一行 lv_* 都不能碰。
        tdeck_set_voice_key_handler([](void* user) {
            static_cast<XiaozhiApp*>(user)->on_voice_key();
        }, this);
    }

    const char* name() const override   { return "XIAOZHI"; }
    const char* icon() const override   { return LV_SYMBOL_VOLUME_MAX; }
    uint32_t    accent() const override { return C_ACCENT; }

    // 小智不出现在应用页 —— 它的入口是首屏头像和数字键 0。
    bool hidden_from_grid() const override { return true; }

    // 常驻后台:切走只收起 UI,连接和音频线程继续活着(ADR-004)
    bool wants_background() const override { return true; }

    void on_enter(lv_obj_t* root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        // 标题放左上角。右上角那一角背光漏光最重(docs/hardware.md),
        // 白底虽然盖得住,但习惯还是留着 —— 以后做深色浮层时这条会救命。
        title_ = lv_label_create(root_);
        lv_obj_set_style_text_font(title_, cjk(), LV_PART_MAIN);
        lv_obj_set_style_text_color(title_, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_label_set_text(title_, "小智");
        lv_obj_set_pos(title_, 14, 8);

        avatar_.create(root_, SCR_W / 2, 92, 132);

        status_ = lv_label_create(root_);
        lv_obj_set_style_text_font(status_, cjk(), LV_PART_MAIN);
        lv_obj_set_style_text_color(status_, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_obj_set_width(status_, SCR_W);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(status_, 0, 162);

        // 字幕卡片。空的时候显示操作提示,有内容时显示最后一句话 ——
        // 两种状态共用一个控件,省得为"没有消息"单独维护一个占位层。
        card_ = lv_obj_create(root_);
        lv_obj_remove_style_all(card_);
        lv_obj_set_pos(card_, 12, 192);
        lv_obj_set_size(card_, SCR_W - 24, 40);
        lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(card_, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_radius(card_, 12, LV_PART_MAIN);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_SCROLLABLE);

        chat_ = lv_label_create(card_);
        lv_obj_set_style_text_font(chat_, cjk(), LV_PART_MAIN);
        lv_obj_set_width(chat_, SCR_W - 44);
        lv_label_set_long_mode(chat_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(chat_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(chat_, LV_ALIGN_CENTER, 0, 0);

        // 内核可能早就在跑了(在别的 app 里被语音键唤起过),
        // 所以进来先按【当前】状态把界面刷成一致的,不能等下一次回调。
        ensure_started();
        apply_state(xz::state());
        paint_chat();
    }

    void on_exit() override
    {
        // 只拆 UI。内核、连接、音频线程全部继续活着 —— 这是 ADR-004 的核心。
        avatar_.destroy();
        root_ = title_ = status_ = card_ = chat_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (ev.key == tdeck::Key::Enter) { xz::toggle_chat(); return true; }
        return false;
    }

    // 全局语音键(轨迹球中键长按)。跑在 BSP 输入任务里 —— 只能调线程安全的东西。
    void on_voice_key()
    {
        ensure_started();
        xz::toggle_chat();
    }

private:
    // 第一次需要小智时才真正把内核拉起来(幂等)。
    // 不在 app_main 里开机就启动:那样即使用户整天不说话,
    // 也要一直占着音频任务的栈和 Opus 的缓冲。
    void ensure_started()
    {
        if (xz::started()) return;

        xz::Callbacks cb;
        // 下面这些回调都在小智主任务里被调用,而且【LVGL 锁已经被持有】,
        // 所以可以直接动控件。判空是必须的:UI 随时可能因为切到别的 app 被拆掉。
        cb.on_state = [this](xz::State s) { apply_state(s); };
        cb.on_status = [this](const std::string& text) {
            status_text_ = text;
            if (status_) lv_label_set_text(status_, text.c_str());
        };
        cb.on_chat = [this](const std::string& role, const std::string& text) {
            // system 角色是内核的内部提示(比如开机那条 user-agent),不占字幕。
            if (role == "system" && !text.empty()) return;
            chat_text_ = text;
            paint_chat();
        };
        cb.on_emotion = [this](const std::string& emotion) {
            ESP_LOGI(TAG, "表情:%s", emotion.c_str());   // 表情映射留到有实际素材时再做
        };
        cb.on_notification = [this](const std::string& text, int ms) {
            (void)ms;
            chat_text_ = text;
            paint_chat();
        };
        xz::set_callbacks(cb);

        if (!xz::start()) ESP_LOGE(TAG, "小智内核没起来");
    }

    void apply_state(xz::State s)
    {
        using S = xz::State;
        // 状态 → 表情。三态足够:在听(瞳孔放大)、在说(更亮的绿)、其它一律待机。
        auto face = (s == S::Listening) ? tdeck::Avatar::State::Listening
                  : (s == S::Speaking)  ? tdeck::Avatar::State::Speaking
                                        : tdeck::Avatar::State::Idle;
        avatar_.set_state(face);

        // 状态行优先用内核给的原文(它比枚举更具体,比如"连接中..."带省略号),
        // 没有的时候用枚举的中文名兜底。
        const char* txt = status_text_.empty() ? xz::state_text(s) : status_text_.c_str();
        if (status_) lv_label_set_text(status_, txt);
    }

    void paint_chat()
    {
        if (!chat_) return;
        if (chat_text_.empty()) {
            lv_obj_set_style_text_color(chat_, lv_color_hex(C_MUTE), LV_PART_MAIN);
            lv_label_set_text(chat_, "按 y 或长按轨迹球开始说话");
        } else {
            lv_obj_set_style_text_color(chat_, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_label_set_text(chat_, chat_text_.c_str());
        }
        lv_obj_align(chat_, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t*     root_ = nullptr, *title_ = nullptr, *status_ = nullptr;
    lv_obj_t*     card_ = nullptr, *chat_ = nullptr;
    tdeck::Avatar avatar_;
    std::string   status_text_;   // 内核给的状态原文,UI 被拆掉时也要留着
    std::string   chat_text_;     // 最后一句字幕,重新进来要能接上
};

}  // namespace

TDECK_REGISTER_APP(XiaozhiApp)
