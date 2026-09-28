// applemusic_app.cc — Apple Music 车载控制器
//
// 屏幕上是"正在播放"+ 三个走带键,数据和命令都走 AMS(见 ams_client.h)。
// 不需要伴侣 App,不需要开发者账号 —— AMS 是 iOS 自带的。
// 封面、歌单、搜索拿不到,那些要等伴侣 App 那一半。
//
// ── 内存(板子上量的)────────────────────────────────────
// 车载变体开机内部堆 79,491,起完 BLE 还剩 ~30,000,WiFi 全程在线。
// 所以这个 app 【不断 WiFi】—— 断了调试服务也跟着没,截图/OTA/看内存全断。
// (“BLE 和 WiFi 装不下”只在日常固件上成立,那是被小智/B站/音乐占掉的。)
//
// ⚠️ 走带键上的符号必须显式用 Montserrat。
// root 上挂的是中文字体,而 LV_SYMBOL_* 是私有区码点,中文字库里没有 ——
// 直接画出来是豆腐块。这个坑在音乐 app 的播放键上踩过一次。

#include "app.h"
#include "ui/fonts.h"
#include "ams_client.h"
#include "hid_service.h"

#include <esp_heap_caps.h>
#include <lvgl.h>
#include <stdio.h>

namespace {

namespace ams = tdeck::ams;
namespace hid = tdeck::hid;

constexpr uint32_t C_BG    = 0xf6f8fa;
constexpr uint32_t C_CARD  = 0xffffff;
constexpr uint32_t C_TEXT  = 0x1f2328;
constexpr uint32_t C_MUTE  = 0x57606a;
constexpr uint32_t C_OK    = 0x1A7F37;
constexpr uint32_t C_BUSY  = 0x1F6FEB;
constexpr uint32_t C_WARN  = 0xB35900;
constexpr uint32_t C_TRACK = 0xd8dee4;

const char* link_text(ams::Link l)
{
    switch (l) {
    case ams::Link::Off:         return "蓝牙未启用";
    case ams::Link::Starting:    return "蓝牙启动中…";
    case ams::Link::Advertising: return "等待 iPhone 连接";
    case ams::Link::Connecting:  return "已连接,正在配对…";
    case ams::Link::Discovering: return "正在查找 Apple Media Service…";
    case ams::Link::NoService:   return "蓝牙异常 / 对面没有 AMS";
    case ams::Link::Ready:       return "已就绪";
    default:                     return "未知状态";
    }
}

uint32_t link_color(ams::Link l)
{
    if (l == ams::Link::Ready) return C_OK;
    if (l == ams::Link::NoService || l == ams::Link::Off) return C_WARN;
    return C_BUSY;
}

void mmss(char* out, size_t n, float sec)
{
    if (sec < 0) sec = 0;
    int t = (int)sec;
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}


// 一条命令有两条出路,优先级是有理由的:
//
//   AMS  —— 走的是 iOS 的媒体遥控通道,作用在【当前活跃的播放 App】上,
//           而且能做 HID 没有的事(shuffle/repeat/喜欢)。
//   HID  —— 系统级媒体键。AMS 拿不到的时候还能用 ——
//           Apple 明说 AMS 不保证一直在(比如系统这会儿没发布它)。
//
// 两条都不通就返回 false,界面据此把按钮灰掉,而不是假装按下去了。
bool dispatch(ams::Cmd c)
{
    ams::Snapshot s;
    ams::snapshot(&s);
    if (s.link == ams::Link::Ready && ams::supports(s, c) && ams::send(c)) return true;

    // 映射得到的才走 HID。AMS 独有的命令(喜欢、切 shuffle)没有对应的媒体键,
    // 映射不了就老实返回 false。
    switch (c) {
    case ams::Cmd::TogglePlay: return hid::tap(hid::Key::PlayPause);
    case ams::Cmd::Play:       return hid::tap(hid::Key::PlayPause);
    case ams::Cmd::Pause:      return hid::tap(hid::Key::PlayPause);
    case ams::Cmd::NextTrack:  return hid::tap(hid::Key::Next);
    case ams::Cmd::PrevTrack:  return hid::tap(hid::Key::Prev);
    case ams::Cmd::VolumeUp:   return hid::tap(hid::Key::VolumeUp);
    case ams::Cmd::VolumeDown: return hid::tap(hid::Key::VolumeDown);
    default:                   return false;
    }
}

class AppleMusicApp : public tdeck::App {
public:
    const char* name() const override        { return "APPLEMUSIC"; }
    const char* icon() const override        { return LV_SYMBOL_AUDIO; }
    uint32_t    card_color() const override  { return 0x9F1239; }
    const char* card_title() const override  { return "Apple Music"; }

    int shortcuts(const tdeck::Shortcut** out) const override
    {
        static const tdeck::Shortcut sc[] = {
            {"ball L/R", "上一首 / 下一首"},
            {"ball 中键", "播放 / 暂停"},
            {"ball U/D", "音量 +/-"},
        };
        *out = sc;
        return sizeof(sc) / sizeof(sc[0]);
    }

    void on_enter(lv_obj_t* root) override
    {
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        // 中文字体在 root 上设一次,文本样式向下继承。
        // 不设的话 lv_label_create 用 LVGL 默认的 Montserrat,中文全是豆腐块。
        const lv_font_t* f16 = tdeck::font_cjk_small();
        const lv_font_t* f20 = tdeck::font_cjk();
        if (f16) lv_obj_set_style_text_font(root, f16, LV_PART_MAIN);

        status_ = lv_label_create(root);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(status_, 288);
        lv_obj_set_pos(status_, 16, 10);

        // ── 正在播放 ──
        card_ = lv_obj_create(root);
        lv_obj_set_size(card_, 288, 110);
        lv_obj_set_pos(card_, 16, 34);
        lv_obj_set_style_radius(card_, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(card_, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(card_, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_pad_all(card_, 12, LV_PART_MAIN);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_SCROLLABLE);
        // lv_obj_create 默认可点,装饰性容器要摘掉,否则会吃掉落在它身上的点击
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_CLICKABLE);

        title_ = lv_label_create(card_);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title_, 264);
        if (f20) lv_obj_set_style_text_font(title_, f20, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_pos(title_, 0, 0);

        artist_ = lv_label_create(card_);
        lv_label_set_long_mode(artist_, LV_LABEL_LONG_DOT);
        lv_obj_set_width(artist_, 264);
        lv_obj_set_style_text_color(artist_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(artist_, 0, 30);

        // ── 进度 ──
        bar_ = lv_bar_create(card_);
        lv_obj_set_size(bar_, 264, 4);
        lv_obj_set_pos(bar_, 0, 56);
        lv_bar_set_range(bar_, 0, 1000);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(C_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(0x9F1239), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar_, 2, LV_PART_INDICATOR);

        t_now_ = lv_label_create(card_);
        lv_obj_set_style_text_color(t_now_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(t_now_, 0, 64);

        t_end_ = lv_label_create(card_);
        lv_obj_set_style_text_color(t_end_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_align(t_end_, LV_ALIGN_TOP_RIGHT, 0, 64);

        // ── 走带键 ──
        btn_prev_ = key(root, 40,  158, LV_SYMBOL_PREV,  ams::Cmd::PrevTrack);
        btn_play_ = key(root, 122, 154, LV_SYMBOL_PLAY,  ams::Cmd::TogglePlay, true);
        btn_next_ = key(root, 220, 158, LV_SYMBOL_NEXT,  ams::Cmd::NextTrack);

        foot_ = lv_label_create(root);
        lv_obj_set_style_text_color(foot_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(foot_, 16, 218);

        ams::start();

        // 250ms:进度是本地按速率外推的,再快也没有新信息,
        // 再慢秒数跳字看得出来。
        tick_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<AppleMusicApp*>(lv_timer_get_user_data(t))->refresh();
        }, 250, this);
        refresh();
    }

    void on_exit() override
    {
        if (tick_) { lv_timer_delete(tick_); tick_ = nullptr; }
        // BLE 是全局资源,借了必须还 —— 不关的话别的 app 白少 57KB 内部 RAM
        ams::stop();
        status_ = card_ = title_ = artist_ = bar_ = t_now_ = t_end_ = nullptr;
        btn_prev_ = btn_play_ = btn_next_ = foot_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        switch (ev.key) {
        case tdeck::Key::Left:  dispatch(ams::Cmd::PrevTrack);  return true;
        case tdeck::Key::Right: dispatch(ams::Cmd::NextTrack);  return true;
        case tdeck::Key::Enter: dispatch(ams::Cmd::TogglePlay); return true;
        case tdeck::Key::Up:    dispatch(ams::Cmd::VolumeUp);   return true;
        case tdeck::Key::Down:  dispatch(ams::Cmd::VolumeDown); return true;
        default: return false;    // Back 交给 launcher,永远退得出去
        }
    }

private:
    struct KeyCtx { ams::Cmd cmd; };

    lv_obj_t* key(lv_obj_t* root, int x, int y, const char* sym, ams::Cmd cmd,
                  bool big = false)
    {
        const int d = big ? 56 : 48;
        lv_obj_t* b = lv_obj_create(root);
        lv_obj_set_size(b, d, d);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, d / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(big ? 0x9F1239 : C_CARD), LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);

        auto* ctx = new KeyCtx{cmd};
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto* c = (KeyCtx*)lv_event_get_user_data(e);
            dispatch(c->cmd);
        }, LV_EVENT_CLICKED, ctx);
        // 控件树由 launcher 连同 screen 一起销毁,ctx 要跟着走,不然每进一次漏一个
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            delete (KeyCtx*)lv_event_get_user_data(e);
        }, LV_EVENT_DELETE, ctx);

        lv_obj_t* l = lv_label_create(b);
        // ⚠️ 必须显式指定 Montserrat:LV_SYMBOL_* 是私有区码点,
        // root 上继承下来的中文字库里没有,会画成豆腐块
        lv_obj_set_style_text_font(l, big ? &lv_font_montserrat_28
                                          : &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(big ? 0xFFFFFF : C_TEXT), LV_PART_MAIN);
        lv_label_set_text(l, sym);
        lv_obj_center(l);
        if (big) play_sym_ = l;
        return b;
    }

    void refresh()
    {
        if (!status_) return;

        ams::Snapshot s;
        ams::snapshot(&s);

        const bool hid_ok = hid::subscribed();
        if (s.link != ams::Link::Ready && hid_ok) {
            // 这是个真实且常见的状态:iPhone 连着、媒体键能用,只是 AMS
            // 这会儿没发布。说"对面没有 AMS"会让人以为整个坏了。
            lv_label_set_text(status_, "媒体键可用 · 拿不到曲目信息");
            lv_obj_set_style_text_color(status_, lv_color_hex(C_BUSY), LV_PART_MAIN);
        } else {
            lv_label_set_text(status_, link_text(s.link));
            lv_obj_set_style_text_color(status_, lv_color_hex(link_color(s.link)), LV_PART_MAIN);
        }

        const bool has = s.title[0] || s.artist[0];
        lv_label_set_text(title_,  has ? s.title  : "—");
        // 专辑名基本没地方放,和艺人并在一行,没有就只显示艺人
        if (has && s.album[0]) {
            lv_label_set_text_fmt(artist_, "%s · %s", s.artist, s.album);
        } else {
            lv_label_set_text(artist_, has ? s.artist : "还没有曲目信息");
        }

        float el = ams::elapsed_now(s);
        char a[16], b[16];
        mmss(a, sizeof(a), el);
        mmss(b, sizeof(b), s.duration_s);
        lv_label_set_text(t_now_, a);
        lv_label_set_text(t_end_, b);
        lv_bar_set_value(bar_, s.duration_s > 0 ? (int)(el / s.duration_s * 1000) : 0,
                         LV_ANIM_OFF);

        const bool playing = (s.state == ams::PLAY_PLAYING);
        lv_label_set_text(play_sym_, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

        // AMS 说不支持,不代表按不了 —— HID 媒体键还在。
        // 只有两条路都不通才灰掉。
        auto usable = [&](ams::Cmd c) {
            return (s.link == ams::Link::Ready && ams::supports(s, c)) || hid_ok;
        };
        dim(btn_prev_, !usable(ams::Cmd::PrevTrack));
        dim(btn_next_, !usable(ams::Cmd::NextTrack));
        dim(btn_play_, !usable(ams::Cmd::TogglePlay));

        // 底部这行在"还没连上"的时候是【该怎么办】,连上了才变成状态信息 ——
        // 第一次用的人最需要知道的是去哪儿配对,不是内部堆还剩多少
        if (s.link == ams::Link::Advertising) {
            // 加了 HID 服务之后 iOS 才会在蓝牙设置里列出它 —— 在那之前
            // 这句话是错的(普通 BLE 外设不管广播多标准都不出现在设置里),
            // 只能靠第三方 BLE 工具配对。现在实测能找到了。
            lv_label_set_text(foot_, "iPhone 的 设置 → 蓝牙 里选 T-Deck");
        } else if (s.link == ams::Link::NoService) {
            lv_label_set_text(foot_, "把 iPhone 解锁,或者先放一首歌");
        } else if (s.player[0]) {
            lv_label_set_text_fmt(foot_, "%s   内部堆 %u", s.player,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        } else {
            lv_label_set_text_fmt(foot_, "内部堆 %u",
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }

    static void dim(lv_obj_t* o, bool off)
    {
        if (o) lv_obj_set_style_opa(o, off ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
    }

    lv_timer_t* tick_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* card_   = nullptr;
    lv_obj_t* title_  = nullptr;
    lv_obj_t* artist_ = nullptr;
    lv_obj_t* bar_    = nullptr;
    lv_obj_t* t_now_  = nullptr;
    lv_obj_t* t_end_  = nullptr;
    lv_obj_t* btn_prev_ = nullptr;
    lv_obj_t* btn_play_ = nullptr;
    lv_obj_t* btn_next_ = nullptr;
    lv_obj_t* play_sym_ = nullptr;
    lv_obj_t* foot_   = nullptr;
};

}  // namespace

TDECK_REGISTER_APP(AppleMusicApp)
