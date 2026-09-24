#include "volume.h"

#include <lvgl.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>
#include <stdio.h>

#include "tdeck_bsp.h"

namespace tdeck::volume {
namespace {

constexpr const char* TAG     = "volume";
constexpr const char* NVS_NS  = "tdecksys";
constexpr const char* NVS_KEY = "vol";
constexpr int DEFAULT_VOL = 70;

int s_vol = DEFAULT_VOL;

// ── HUD ──────────────────────────────────────────────────────
// 建在 lv_layer_top() 上,盖在任何 app 之上 —— 音量是系统级的,
// 不该被 app 的根容器裁掉。
lv_obj_t*  s_hud      = nullptr;
lv_obj_t*  s_hud_fill = nullptr;
lv_obj_t*  s_hud_num  = nullptr;
lv_timer_t* s_hide    = nullptr;
lv_timer_t* s_save    = nullptr;

constexpr int HUD_W = 184, HUD_H = 38, BAR_W = 96;

struct Sub { Listener cb; void* user; };
Sub s_subs[4];

void notify()
{
    for (auto& s : s_subs) if (s.cb) s.cb(s_vol, s.user);
}

void save_cb(lv_timer_t*)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY, s_vol);
        nvs_commit(h);
        nvs_close(h);
    }
    s_save = nullptr;
}

void hide_cb(lv_timer_t*)
{
    if (s_hud) { lv_obj_delete(s_hud); s_hud = nullptr; s_hud_fill = s_hud_num = nullptr; }
    s_hide = nullptr;
}

void build_hud()
{
    s_hud = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_hud, HUD_W, HUD_H);
    // 贴顶不贴底:底部在音乐播放页是控制键那一行,盖住等于挡住手要按的地方。
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(s_hud, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_hud, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_hud, HUD_H / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_hud, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_hud, lv_color_hex(0xE2E7DC), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_hud, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_hud, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_hud, lv_color_hex(0x55853A), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_hud, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_hud, LV_OBJ_FLAG_CLICKABLE);   // 别挡住底下 app 的点击

    lv_obj_t* ic = lv_label_create(s_hud);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x55853A), LV_PART_MAIN);
    lv_label_set_text(ic, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t* track = lv_obj_create(s_hud);
    lv_obj_set_size(track, BAR_W, 6);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xE2E7DC), LV_PART_MAIN);
    lv_obj_set_style_radius(track, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    s_hud_fill = lv_obj_create(track);
    lv_obj_set_size(s_hud_fill, 0, 6);
    lv_obj_set_pos(s_hud_fill, 0, 0);
    lv_obj_set_style_bg_color(s_hud_fill, lv_color_hex(0x55853A), LV_PART_MAIN);
    lv_obj_set_style_radius(s_hud_fill, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_hud_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_hud_fill, 0, LV_PART_MAIN);

    s_hud_num = lv_label_create(s_hud);
    lv_obj_set_style_text_font(s_hud_num, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hud_num, lv_color_hex(0x6A7360), LV_PART_MAIN);
    lv_obj_align(s_hud_num, LV_ALIGN_RIGHT_MID, -12, 0);
}

// 调用方必须已经持有 LVGL 锁 —— 按键分发和滑块回调都是在锁里进来的。
void show_hud()
{
    if (!s_hud) build_hud();
    lv_obj_move_foreground(s_hud);

    lv_obj_set_width(s_hud_fill, BAR_W * s_vol / 100);
    char b[8];
    snprintf(b, sizeof(b), "%d", s_vol);
    lv_label_set_text(s_hud_num, b);

    if (s_hide) lv_timer_delete(s_hide);
    s_hide = lv_timer_create(hide_cb, 1400, nullptr);
    lv_timer_set_repeat_count(s_hide, 1);
}

// NVS 写入要攒一下。按住 O 连点十几下就写十几次 flash,没必要。
void schedule_save()
{
    if (s_save) lv_timer_delete(s_save);
    s_save = lv_timer_create(save_cb, 2000, nullptr);
    lv_timer_set_repeat_count(s_save, 1);
}

}  // namespace

void init()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = DEFAULT_VOL;
        if (nvs_get_i32(h, NVS_KEY, &v) == ESP_OK && v >= 0 && v <= 100) s_vol = (int)v;
        nvs_close(h);
    }
    tdeck_speaker_set_volume((uint8_t)s_vol);
    ESP_LOGI(TAG, "音量 %d", s_vol);
}

int get() { return s_vol; }

void set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_vol) return;
    s_vol = percent;
    tdeck_speaker_set_volume((uint8_t)s_vol);
    show_hud();
    schedule_save();
    notify();
}

void step(int delta)
{
    // 固定 5 一档,并且先对齐到 5 的格子上。不对齐的话,从 72 往下按会
    // 直接掉到 65(跨了 7),手感不均匀。
    int v = delta > 0 ? (s_vol / 5) * 5 + 5
                      : ((s_vol + 4) / 5) * 5 - 5;
    set(v);
}

void subscribe(Listener cb, void* user)
{
    for (auto& s : s_subs) if (!s.cb) { s = {cb, user}; return; }
    ESP_LOGW(TAG, "订阅者满了");
}

void unsubscribe(void* user)
{
    for (auto& s : s_subs) if (s.user == user) s = {nullptr, nullptr};
}

}  // namespace tdeck::volume
