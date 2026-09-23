// main.cc — M1 第一个里程碑:上电 → 点亮屏幕 → LVGL 出画面
//
// 这一版只验证最底层的链路:GPIO10 上电、SPI/ST7789 初始化、LVGL 能画东西。
// 触摸、键盘、轨迹球、音频在后续提交里加,每加一个,自检应用里多亮一站
// (见 docs/apps/selftest.md)。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char* TAG = "main";

static lv_display_t* s_disp;

static void ui_bringup_screen()
{
    lvgl_port_lock(0);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "T-Deck OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0x58a6ff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "BSP bring-up");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 5);

    // 电池电压 —— 顺便验证 ADC 通了
    int mv = tdeck_battery_mv();
    lv_obj_t* bat = lv_label_create(scr);
    if (mv > 0) lv_label_set_text_fmt(bat, "battery  %d.%02d V", mv / 1000, (mv % 1000) / 10);
    else        lv_label_set_text(bat, "battery  n/a");
    lv_obj_set_style_text_color(bat, lv_color_hex(0x3fb950), LV_PART_MAIN);
    lv_obj_align(bat, LV_ALIGN_CENTER, 0, 35);

    // 非对称标记:用来确认 swap_xy / mirror 的组合对不对。
    // 这个小方块必须出现在【左上角】。跑到右上角说明水平镜像反了,
    // 跑到左下角说明垂直镜像反了 —— 光看代码看不出来,必须上真机。
    lv_obj_t* corner = lv_obj_create(scr);
    lv_obj_set_size(corner, 16, 16);
    lv_obj_set_style_bg_color(corner, lv_color_hex(0xf85149), LV_PART_MAIN);
    lv_obj_set_style_border_width(corner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(corner, 0, LV_PART_MAIN);
    lv_obj_align(corner, LV_ALIGN_TOP_LEFT, 0, 0);

    lvgl_port_unlock();
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(tdeck_bsp_init());

    // LVGL 接到面板上。缓冲走 PSRAM(8MB 很宽裕),双缓冲让刷新不撕裂。
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = tdeck_get_panel_io(),
        .panel_handle  = tdeck_get_panel(),
        .buffer_size   = TDECK_LCD_H_RES * 40,
        .double_buffer = true,
        .hres          = TDECK_LCD_H_RES,
        .vres          = TDECK_LCD_V_RES,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = false,   // 已经在 panel 层做了,这里不要重复
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = false,
            .buff_spiram = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    assert(s_disp);

    ui_bringup_screen();

    // 背光渐亮 —— 直接全亮会有一瞬间的白闪
    for (int p = 0; p <= 80; p += 4) {
        tdeck_backlight_set(p);
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    ESP_LOGI(TAG, "bring-up 完成");
}
