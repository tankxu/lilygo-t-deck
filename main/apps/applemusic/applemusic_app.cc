// applemusic_app.cc — Apple Music 车载控制器
//
// 现在这一版只做到「把 BLE 协议栈起起来」。AMS 客户端(配对 + GATT 发现 +
// Remote Command / Entity Update)还没接。
//
// ── 内存账(全是这块板子上量的)────────────────────────────
//
// 一开始以为 BLE 和 WiFi 装不下,得进 app 先断网。那个结论是在【日常固件】
// 上量的,不成立于这里:
//
//   日常固件 + BT      开机空闲 19,267   → 起 BLE 失败
//   车载固件 + BT      开机空闲 79,491   → 起 BLE 成功,之后还剩 30,483
//
// 差别不在蓝牙,在【被砍掉的那几个 app】:车载变体不编小智/B站/音乐,
// .dram0.bss 从 0xe570 掉到 0x7fd8(省 26KB),再加上 esp-sr 那套不再常驻。
// 也就是说 "BLE 挤掉 WiFi" 是日常固件的现象,不是芯片的限制。
//
// 所以这个 app 【不断 WiFi】。好处不只是简单:断网会把调试服务一起收掉,
// 截图/OTA/看内存全没了 —— 开发期基本等于闭着眼睛写。
//
// ⚠️ 留了一条后路:如果接上 AMS 之后 30KB 不够用(GATT 发现 + 界面还要吃),
// net_suspend() / net_resume() 是现成的,一次能再腾出 69KB。
// 但那要等真的不够再说,别提前付这个代价。

#include "app.h"
#include "ui/fonts.h"
#include "net/net.h"
#include "debug/ble_probe.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <lvgl.h>
#include <stdio.h>

namespace {

const char* TAG = "applemusic";

constexpr uint32_t C_BG   = 0xf6f8fa;
constexpr uint32_t C_CARD = 0xffffff;
constexpr uint32_t C_TEXT = 0x1f2328;
constexpr uint32_t C_MUTE = 0x57606a;
constexpr uint32_t C_OK   = 0x1A7F37;
constexpr uint32_t C_WARN = 0xB35900;

class AppleMusicApp : public tdeck::App {
public:
    const char* name() const override        { return "APPLEMUSIC"; }
    const char* icon() const override        { return LV_SYMBOL_AUDIO; }
    uint32_t    card_color() const override  { return 0x9F1239; }
    const char* card_title() const override  { return "Apple Music"; }

    void on_enter(lv_obj_t* root) override
    {
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        // ⚠️ 字体要在 root 上设一次 —— LVGL 的文本样式会向下继承。
        // 不设的话 lv_label_create 用的是 LVGL 默认的 Montserrat(只有 ASCII),
        // 中文全变豆腐块,而且不报任何错。
        const lv_font_t* f = tdeck::font_cjk_small();
        if (f) lv_obj_set_style_text_font(root, f, LV_PART_MAIN);

        lv_obj_t* t = lv_label_create(root);
        lv_label_set_text(t, "Apple Music");
        lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_pos(t, 16, 12);

        state_ = lv_label_create(root);
        lv_obj_set_pos(state_, 16, 44);

        card_ = lv_obj_create(root);
        lv_obj_set_size(card_, 288, 74);
        lv_obj_set_pos(card_, 16, 76);
        lv_obj_set_style_radius(card_, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(card_, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(card_, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_pad_all(card_, 10, LV_PART_MAIN);
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_SCROLLABLE);
        // lv_obj_create 建出来的对象【默认可点】,装饰性容器必须摘掉,
        // 否则它会把落在自己身上的点击吃掉(music/settings/bilibili 各踩过一次)
        lv_obj_remove_flag(card_, LV_OBJ_FLAG_CLICKABLE);

        track_ = lv_label_create(card_);
        lv_label_set_text(track_, "—");
        lv_obj_set_style_text_color(track_, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_pos(track_, 0, 0);

        artist_ = lv_label_create(card_);
        lv_label_set_text(artist_, "还没连上 iPhone");
        lv_obj_set_style_text_color(artist_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(artist_, 0, 26);

        heap_ = lv_label_create(root);
        lv_obj_set_style_text_color(heap_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(heap_, 16, 166);

        note_ = lv_label_create(root);
        lv_label_set_long_mode(note_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note_, 288);
        lv_obj_set_style_text_color(note_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(note_, 16, 192);
        lv_label_set_text(note_, "AMS 客户端还没接上 —— 现在只把蓝牙协议栈起起来。");

        // 起 BLE。nimble_port_init() 是同步的而且很快(几毫秒),
        // 不值得为它开任务 —— 开了反而要处理"界面已经建好但状态还没回来"。
        up_ = tdeck::ble_probe_up();
        if (!up_) ESP_LOGE(TAG, "BLE 起不来,内部堆 %u",
                           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        tick_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<AppleMusicApp*>(lv_timer_get_user_data(t))->refresh();
        }, 1000, this);
        refresh();
    }

    void on_exit() override
    {
        if (tick_) { lv_timer_delete(tick_); tick_ = nullptr; }
        // BLE 是全局资源,借了必须还 —— 留着不关,别的 app 就白少 57KB
        tdeck::ble_probe_down();
        up_ = false;
        state_ = card_ = track_ = artist_ = heap_ = note_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        (void)ev;
        return false;   // Back 交给 launcher,永远能退出去
    }

private:
    void refresh()
    {
        if (!state_) return;
        lv_label_set_text(state_, up_ ? "蓝牙已启用 · 等待 iPhone 连接"
                                      : "蓝牙启动失败");
        lv_obj_set_style_text_color(state_, lv_color_hex(up_ ? C_OK : C_WARN),
                                    LV_PART_MAIN);
        lv_label_set_text_fmt(heap_, "内部堆 %u  最大块 %u  WiFi %s",
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                              tdeck::net_suspended() ? "已挂起" : "在线");
    }

    bool        up_ = false;
    lv_timer_t* tick_ = nullptr;
    lv_obj_t*   state_  = nullptr;
    lv_obj_t*   card_   = nullptr;
    lv_obj_t*   track_  = nullptr;
    lv_obj_t*   artist_ = nullptr;
    lv_obj_t*   heap_   = nullptr;
    lv_obj_t*   note_   = nullptr;
};

}  // namespace

TDECK_REGISTER_APP(AppleMusicApp)
