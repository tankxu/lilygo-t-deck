// xiaozhi_app.cc — 小智
//
// 入口有三个,都不是应用页上的卡片(ADR-004:小智是常驻角色不是普通应用):
//   · 首屏左上角的头像
//   · 键盘数字键 0(任何界面下都生效,由 Host 全局截获)
//   · 轨迹球中键长按(全局语音键,任何 app 里都能唤起)
//
// 这一层只管 UI 和状态机接线。音频链路和协议栈在 components/xiaozhi 里,
// 通过 xiaozhi_core.h 的几个函数对接 —— 界面不该知道 Opus 和 WebSocket 的存在。
//
// ── 界面为什么建在 lv_layer_top() 上(ADR-004)──
//
// 小智的控件树【不属于任何一块 screen】。app 切换是 lv_screen_load_anim,
// 旧 screen 会被 launcher 删掉;如果把小智的控件挂在 screen 上,
// 切走的瞬间控件就被连根删除,而内核还在后台说话、还会回调进来改字幕 ——
// 那就是野指针。lv_layer_top() 是唯一不随 screen 切换销毁的层。
//
// 于是"收起"只是给面板加一个 HIDDEN 标志,零销毁零重建,也就不存在
// "回来时要重新建 UI"的问题 —— 后台说了什么,切回来一眼就能看到。
//
// 同一个面板有两种形态:
//   · Full    进入小智应用时的整页形态(大脸)
//   · Overlay 在别的 app 里被语音键唤起时的悬浮卡片(小脸,不挡住底下的事)
// 形态切换只改几何尺寸,不重建控件树(脸除外 —— 它的尺寸是创建时定死的)。

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

// 整机浅色系。深色会把屏幕四角的背光漏光衬出来(docs/hardware.md),
// 只有【脸本身】是深色 —— 亮绿的眼睛需要深底才发得出光,而那只是一个圆。
constexpr uint32_t C_BG     = 0xFFFFFF;
constexpr uint32_t C_TEXT   = 0x1B2117;
constexpr uint32_t C_MUTE   = 0x8A9480;
constexpr uint32_t C_ACCENT = 0x55853A;   // 军绿,主题色
constexpr uint32_t C_CARD   = 0xF2F5EE;   // 极浅的军绿灰,和主题色同色相
constexpr uint32_t C_LINE   = 0xE3E9DC;

constexpr int SCR_W = 320, SCR_H = 240;

// 悬浮卡片的几何。放在【偏下偏左】:右上角是漏光最重的一角,
// 而底部正好是用户视线从键盘抬起来第一眼看到的地方。
constexpr int OV_X = 10, OV_Y = 118, OV_W = 300, OV_H = 112;

// 中文字体能不能【真的画出字】。
//
// 光看 font_cjk() 非空不够:cbin 字库是把结构体裸 dump 出来的,LVGL 小版本一变
// 字段布局就对不上。实测过一种最坑的中间态 —— 'A' 的 box_w 查得出来(所以
// fonts.cc 的自检通过),但每个字形只画出【最后一行像素】,屏幕上是一条条
// 1px 的横杠,看起来像"字被压扁了",完全不像字体问题。
//
// 所以这里再验一步高度:查一个汉字,box_h 小于 8 就判定这个字库在当前 LVGL 下
// 不可用,整个 UI 退回 Montserrat + 英文。宁可显示英文,也不要显示一堆横杠。
static bool cjk_ok()
{
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const lv_font_t* f = tdeck::font_cjk();
    cached = 0;
    if (f) {
        lv_font_glyph_dsc_t g = {};
        // U+4F60 "你" —— 随便挑的常用字,GB2312 里一定有
        if (lv_font_get_glyph_dsc(f, &g, 0x4F60, 0) && g.box_w > 0 && g.box_h >= 8) cached = 1;
    }
    ESP_LOGW(TAG, "中文字体%s可用(小智界面%s)",
             cached ? "" : "不", cached ? "用中文" : "退回英文");
    return cached == 1;
}

static const lv_font_t* cjk()
{
    const lv_font_t* f = tdeck::font_cjk();
    return (f && cjk_ok()) ? f : &lv_font_montserrat_20;
}

// 中文字体不可用时,状态和提示全部退回英文 —— 缺字形的中文串在 Montserrat 下
// 是什么都不画,一片空白比横杠还难懂。
static const char* state_en(xz::State s)
{
    using S = xz::State;
    switch (s) {
    case S::Starting:        return "starting";
    case S::WifiConfiguring: return "wifi setup";
    case S::Idle:            return "standby";
    case S::Connecting:      return "connecting";
    case S::Listening:       return "listening";
    case S::Speaking:        return "speaking";
    case S::Upgrading:       return "upgrading";
    case S::Activating:      return "activating";
    case S::AudioTesting:    return "audio test";
    case S::FatalError:      return "error";
    default:                 return "";
    }
}

// 文本里有没有非 ASCII。中文字体不可用时,内核给的中文原文一个字也画不出来,
// 显示出来就是空白 —— 那还不如用我们自己的英文状态名。
static bool has_cjk(const std::string& s)
{
    for (unsigned char c : s) if (c >= 0x80) return true;
    return false;
}

enum class Mode { Hidden, Full, Overlay };

class XiaozhiApp : public tdeck::App {
public:
    XiaozhiApp()
    {
        // 全局语音键(ADR-004:轨迹球中键长按 600ms)。在构造函数里注册 =
        // 开机就生效,不需要先进过一次小智 —— 这正是"常驻"的含义。
        // 这个回调跑在 BSP 的输入任务里,不是 LVGL 任务,所以里面只能调
        // xz:: 那几个线程安全的函数,一行 lv_* 都不能碰。
        tdeck_set_voice_key_handler([](void* user) {
            static_cast<XiaozhiApp*>(user)->on_voice_key();
        }, this);
    }

    const char* name() const override   { return "XIAOZHI"; }
    const char* icon() const override   { return LV_SYMBOL_VOLUME_MAX; }
    uint32_t    accent() const override { return C_ACCENT; }
    bool hidden_from_grid() const override { return true; }

    // 常驻后台:切走只收起 UI,连接和音频线程继续活着(ADR-004)
    bool wants_background() const override { return true; }

    void on_enter(lv_obj_t* root) override
    {
        // 底下这块 screen 故意留白:真正的界面在 lv_layer_top() 上。
        // 留白是有意义的 —— 面板万一没建起来,用户看到的是干净的白屏而不是花屏。
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        in_app_ = true;
        ensure_started();
        show(Mode::Full);
    }

    void on_exit() override
    {
        // 只收起【整页】。内核、连接、音频线程全部继续活着 —— 这是 ADR-004 的核心。
        in_app_ = false;

        // 正在对话时切走,不是关掉它,而是【降级成悬浮窗】:
        // 话说到一半因为用户去开了个别的 app 就消失,是最让人恼火的一种交互。
        // 真正空闲了才整个收起来。
        using S = xz::State;
        auto s = xz::state();
        if (s == S::Listening || s == S::Speaking || s == S::Connecting || s == S::Activating)
            show(Mode::Overlay);
        else
            hide();
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (ev.key == tdeck::Key::Enter) { xz::toggle_chat(); return true; }
        return false;
    }

    // 全局语音键。跑在 BSP 输入任务里 —— 只能调线程安全的东西。
    // 面板由随后的状态回调负责浮出来(那时 LVGL 锁在内核手上,是安全的)。
    void on_voice_key()
    {
        ensure_started();
        xz::toggle_chat();
    }

private:
    // ── 内核 ───────────────────────────────────────────────
    // 第一次需要小智时才真正把内核拉起来(幂等)。不在 app_main 里开机就启动:
    // 那样即使用户整天不说话,也要一直占着音频任务的栈和 Opus 的缓冲。
    void ensure_started()
    {
        if (xz::started()) return;

        xz::Callbacks cb;
        // 这些回调都在小智主任务里被调用,而且【LVGL 锁已经被 XzDisplay 持有】,
        // 所以可以直接动控件。每一处都判空:面板可能还没建起来。
        cb.on_state  = [this](xz::State s) { apply_state(s); };
        cb.on_status = [this](const std::string& t) {
            status_text_ = t;
            if (status_ && (cjk_ok() || !has_cjk(t))) lv_label_set_text(status_, t.c_str());
        };
        cb.on_chat = [this](const std::string& role, const std::string& t) {
            // system 角色是内核的内部提示(比如开机那条 user-agent),不占字幕
            if (role == "system" && !t.empty()) return;
            chat_text_ = t;
            paint_chat();
        };
        cb.on_emotion = [](const std::string& e) {
            ESP_LOGI(TAG, "表情:%s", e.c_str());   // 表情映射留到有素材时再做
        };
        cb.on_notification = [this](const std::string& t, int ms) {
            (void)ms; chat_text_ = t; paint_chat();
        };
        xz::set_callbacks(cb);

        if (!xz::start()) ESP_LOGE(TAG, "小智内核没起来");
    }

    // ── 面板 ───────────────────────────────────────────────
    // 建在 lv_layer_top() 上,建一次用一辈子,只隐藏不销毁。
    void build_panel()
    {
        if (panel_) return;

        lv_obj_t* top = lv_layer_top();

        // 全屏容器。Overlay 形态下它是【完全透明且不可点】的 ——
        // 不加这一条,整个 top layer 会把触摸全吃掉,底下的 app 就点不动了。
        panel_ = lv_obj_create(top);
        lv_obj_remove_style_all(panel_);
        lv_obj_set_pos(panel_, 0, 0);
        lv_obj_set_size(panel_, SCR_W, SCR_H);
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);

        // 卡片。Full 形态下铺满整屏并去掉圆角阴影,Overlay 形态下是一张悬浮卡。
        card_ = lv_obj_create(panel_);
        lv_obj_remove_style_all(card_);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);   // 吃掉落在卡片上的点击
        lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(card_, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_border_width(card_, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(card_, lv_color_hex(C_LINE), LV_PART_MAIN);

        // 状态行做成一个 flex 行:状态灯 + 文字。
        // 不用"自己算坐标"是因为文字宽度随状态变(standby / connecting / listening),
        // 手算偏移必然在某个状态下把灯和字拉开或叠上 —— 交给 flex 一劳永逸。
        row_ = lv_obj_create(card_);
        lv_obj_remove_style_all(row_);
        lv_obj_remove_flag(row_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(row_, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row_, 8, LV_PART_MAIN);

        // 状态灯:一个 8px 的圆点,颜色跟着状态走。
        // 比纯文字快 —— 余光扫一眼就知道它在听还是在说。
        dot_ = lv_obj_create(row_);
        lv_obj_remove_style_all(dot_);
        lv_obj_set_size(dot_, 8, 8);
        lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot_, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        // 圆点在文字里居中:flex 的交叉轴对齐管不到单个子项的基线,直接给上边距
        lv_obj_set_style_margin_top(dot_, 9, LV_PART_MAIN);

        status_ = lv_label_create(row_);
        lv_obj_set_style_text_font(status_, cjk(), LV_PART_MAIN);
        lv_obj_set_style_text_color(status_, lv_color_hex(C_ACCENT), LV_PART_MAIN);
        lv_label_set_text(status_, cjk_ok() ? "待命" : "standby");

        // 字幕区。整页形态下给它一块极浅的底,让"这是它说的话"和状态行分开;
        // 悬浮形态下底是卡片本身,这一块就透明。
        chatbox_ = lv_obj_create(card_);
        lv_obj_remove_style_all(chatbox_);
        lv_obj_remove_flag(chatbox_, LV_OBJ_FLAG_SCROLLABLE);

        chat_ = lv_label_create(chatbox_);
        lv_obj_set_style_text_font(chat_, cjk(), LV_PART_MAIN);
        lv_label_set_long_mode(chat_, LV_LABEL_LONG_DOT);

        ESP_LOGI(TAG, "悬浮面板已建在 lv_layer_top()");
    }

    // 形态切换:只改几何,不重建控件树。脸是例外 —— 它的直径是创建时定死的,
    // 两种形态差了一倍(132 / 64),只能重建;但这只发生在形态切换的瞬间,
    // 隐藏/浮出是不重建的。
    void layout(Mode m)
    {
        if (m == Mode::Full) {
            lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(panel_, lv_color_hex(C_BG), LV_PART_MAIN);

            lv_obj_set_pos(card_, 0, 0);
            lv_obj_set_size(card_, SCR_W, SCR_H);
            lv_obj_set_style_radius(card_, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(card_, 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(card_, 0, LV_PART_MAIN);

            if (mode_ != Mode::Full) { avatar_.destroy(); avatar_.create(card_, SCR_W / 2, 88, 128); }

            // 状态行整行居中:宽度给满屏,flex 主轴居中,灯和字自己找位置
            lv_obj_set_width(row_, SCR_W);
            lv_obj_set_pos(row_, 0, 158);
            lv_obj_set_flex_align(row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_width(status_, LV_SIZE_CONTENT);

            lv_obj_set_pos(chatbox_, 12, 190);
            lv_obj_set_size(chatbox_, SCR_W - 24, 44);
            lv_obj_set_style_bg_opa(chatbox_, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(chatbox_, lv_color_hex(C_CARD), LV_PART_MAIN);
            lv_obj_set_style_radius(chatbox_, 12, LV_PART_MAIN);
            lv_obj_set_width(chat_, SCR_W - 44);
            lv_obj_set_style_text_align(chat_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(chat_, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_set_style_bg_opa(panel_, LV_OPA_TRANSP, LV_PART_MAIN);

            lv_obj_set_pos(card_, OV_X, OV_Y);
            lv_obj_set_size(card_, OV_W, OV_H);
            lv_obj_set_style_radius(card_, 18, LV_PART_MAIN);
            lv_obj_set_style_border_width(card_, 1, LV_PART_MAIN);
            // 阴影是"浮在上面"这件事唯一的视觉线索(底下也是浅色),不能省。
            // 用带绿的深灰而不是纯黑,免得在白底上显脏。
            lv_obj_set_style_shadow_width(card_, 24, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(card_, LV_OPA_30, LV_PART_MAIN);
            lv_obj_set_style_shadow_offset_y(card_, 6, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(card_, lv_color_hex(0x6E7A63), LV_PART_MAIN);

            if (mode_ != Mode::Overlay) { avatar_.destroy(); avatar_.create(card_, 54, OV_H / 2, 64); }

            lv_obj_set_width(row_, OV_W - 110);
            lv_obj_set_pos(row_, 98, 16);
            lv_obj_set_flex_align(row_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_width(status_, LV_SIZE_CONTENT);

            lv_obj_set_pos(chatbox_, 98, 46);
            lv_obj_set_size(chatbox_, OV_W - 110, 52);
            lv_obj_set_style_bg_opa(chatbox_, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_width(chat_, OV_W - 114);
            lv_obj_set_style_text_align(chat_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_align(chat_, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        mode_ = m;
        paint_chat();
    }

    void show(Mode m)
    {
        build_panel();
        cancel_autohide();
        if (m != mode_) layout(m);
        if (lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
            // 淡入 + 上浮 12px。悬浮窗突然"啪"地出现会吓人,
            // 180ms 的位移正好够眼睛跟上,又不至于让人等。
            lv_obj_set_style_opa(card_, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, card_);
            lv_anim_set_duration(&a, 180);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
                lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, LV_PART_MAIN);
            });
            lv_anim_start(&a);

            if (m == Mode::Overlay) {
                lv_anim_t b;
                lv_anim_init(&b);
                lv_anim_set_var(&b, card_);
                lv_anim_set_duration(&b, 180);
                lv_anim_set_values(&b, OV_Y + 12, OV_Y);
                lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
                lv_anim_set_exec_cb(&b, [](void* o, int32_t v) {
                    lv_obj_set_y((lv_obj_t*)o, v);
                });
                lv_anim_start(&b);
            }
        }
        lv_obj_move_foreground(panel_);
    }

    void hide()
    {
        cancel_autohide();
        if (panel_) lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        mode_ = Mode::Hidden;
    }

    // 悬浮形态下对话结束就自己退场 —— 用户当时在做别的事,
    // 一张永远赖着不走的卡片就成了遮挡。整页形态不自动退(那是用户主动进来的)。
    void schedule_autohide()
    {
        cancel_autohide();
        autohide_ = lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<XiaozhiApp*>(lv_timer_get_user_data(t));
            self->autohide_ = nullptr;
            lv_timer_delete(t);
            if (!self->in_app_) self->hide();
        }, 3500, this);
        lv_timer_set_repeat_count(autohide_, 1);
    }

    void cancel_autohide()
    {
        if (autohide_) { lv_timer_delete(autohide_); autohide_ = nullptr; }
    }

    void apply_state(xz::State s)
    {
        using S = xz::State;
        bool busy = (s != S::Idle && s != S::Unknown);

        // 面板的浮出时机:内核一旦动起来就露面。在小智应用里是整页形态,
        // 在别的 app 里是悬浮形态 —— 判据就是当前有没有进过 on_enter。
        if (busy) show(in_app_ ? Mode::Full : Mode::Overlay);
        else if (mode_ == Mode::Overlay) schedule_autohide();

        if (!panel_) return;

        auto face = (s == S::Listening) ? tdeck::Avatar::State::Listening
                  : (s == S::Speaking)  ? tdeck::Avatar::State::Speaking
                                        : tdeck::Avatar::State::Idle;
        avatar_.set_state(face);

        uint32_t c = (s == S::Listening)  ? C_ACCENT
                   : (s == S::Speaking)   ? 0x3F7D1F
                   : (s == S::FatalError) ? 0xC0562F
                                          : C_MUTE;
        if (dot_)    lv_obj_set_style_bg_color(dot_, lv_color_hex(c), LV_PART_MAIN);
        if (status_) lv_obj_set_style_text_color(status_, lv_color_hex(c), LV_PART_MAIN);

        // 状态行优先用内核给的原文(它比枚举更具体,比如"连接中..."带省略号);
        // 没有、或者画不出中文时,退回我们自己的状态名。
        const char* txt;
        if (!cjk_ok())                       txt = state_en(s);
        else if (status_text_.empty())       txt = xz::state_text(s);
        else                                 txt = status_text_.c_str();
        if (status_) lv_label_set_text(status_, txt);
    }

    void paint_chat()
    {
        if (!chat_) return;
        if (chat_text_.empty()) {
            lv_obj_set_style_text_color(chat_, lv_color_hex(C_MUTE), LV_PART_MAIN);
            if (!cjk_ok())
                lv_label_set_text(chat_, mode_ == Mode::Full ? "press y or hold the ball to talk"
                                                            : "hold the ball to talk");
            else
                lv_label_set_text(chat_, mode_ == Mode::Full ? "按 y 或长按轨迹球开始说话"
                                                            : "长按轨迹球说话");
        } else if (!cjk_ok() && has_cjk(chat_text_)) {
            // 画不出来就别装作画得出来 —— 空着更诚实,也不会留下上一句话的残影
            lv_obj_set_style_text_color(chat_, lv_color_hex(C_MUTE), LV_PART_MAIN);
            lv_label_set_text(chat_, "(CJK font unavailable)");
        } else {
            lv_obj_set_style_text_color(chat_, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_label_set_text(chat_, chat_text_.c_str());
        }
    }

    lv_obj_t*     panel_ = nullptr, *card_ = nullptr, *row_ = nullptr, *chatbox_ = nullptr;
    lv_obj_t*     status_ = nullptr, *chat_ = nullptr, *dot_ = nullptr;
    lv_timer_t*   autohide_ = nullptr;
    tdeck::Avatar avatar_;
    Mode          mode_ = Mode::Hidden;
    bool          in_app_ = false;     // 用户是不是正开着小智这个"应用"
    std::string   status_text_;
    std::string   chat_text_;
};

}  // namespace

TDECK_REGISTER_APP(XiaozhiApp)
