// main.cc — 启动:BSP → LVGL → 进入第一个注册的 app
//
// launcher 还没写(M2),所以这里暂时直接进注册表里的第一个 app。
// 等 launcher 到位,这段换成「加载 launcher screen」,其余不用动。

#include "app.h"
#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char* TAG = "main";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(tdeck_bsp_init());

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
        // ⚠️ 这三个值必须跟 tdeck_display_init() 里设的一致。
        // lvgl_port_add_disp() 结尾会调 lvgl_port_disp_rotation_update(),
        // 无条件用这里的值去 esp_lcd_panel_swap_xy/mirror 覆盖面板设置 ——
        // 留 false 的话面板会被打回 240x320 竖屏,而 LVGL 仍按 320 宽喂数据,
        // 每行错位累积,画面就成了竖条纹。
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            // 缓冲必须放内部 RAM 且具备 DMA 能力。放 PSRAM 会花屏:
            // esp_lcd 的 SPI 通道要对缓冲做 DMA,而 ESP32-S3 的 SPI DMA 读 PSRAM
            // 有对齐和 cache 同步的约束。双缓冲共 50KB,内部 RAM 放得下。
            .buff_dma    = true,
            .buff_spiram = false,
            // SPI 屏收的是大端 RGB565,LVGL 渲染出来是小端,必须翻字节序。
            .swap_bytes  = true,
        },
    };
    lv_display_t* disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);

    auto& apps = tdeck::AppRegistry::instance().apps();
    ESP_LOGI(TAG, "已注册 %d 个 app", (int)apps.size());
    for (auto* a : apps) ESP_LOGI(TAG, "    - %s", a->name());
    if (apps.empty()) {
        ESP_LOGE(TAG, "注册表是空的 —— 检查 main/CMakeLists.txt 有没有 WHOLE_ARCHIVE");
    }

    lvgl_port_lock(0);
    tdeck::launcher_begin();
    lvgl_port_unlock();

    // 白底把背光漏光盖住了,不必为此压低亮度
    tdeck_backlight_set(70);
}
