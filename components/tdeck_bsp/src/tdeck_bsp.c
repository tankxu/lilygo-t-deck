// tdeck_bsp.c — BSP 总入口
//
// 初始化顺序是有讲究的,不能随便调:
//   1. 外设总电源(GPIO10)—— 不先上电,后面全都认不到
//   2. 等 100ms 让外设稳定
//   3. 显示(顺带停放共享 SPI 上的 SD 卡和 LoRa)
//   4. 之后才是 I2C 上的键盘/触摸/ES7210

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_check.h>
#include <esp_log.h>

static const char* TAG = "tdeck_bsp";

esp_err_t tdeck_bsp_init(void)
{
    ESP_LOGI(TAG, "T-Deck BSP 初始化开始");

    ESP_RETURN_ON_ERROR(tdeck_power_on(), TAG, "外设上电失败");

    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    ESP_RETURN_ON_ERROR(tdeck_display_init(&panel, &io), TAG, "显示初始化失败");

    int mv = tdeck_battery_mv();
    if (mv > 0) ESP_LOGI(TAG, "电池 %d mV", mv);
    else        ESP_LOGW(TAG, "电池电压读不到");

    ESP_LOGI(TAG, "T-Deck BSP 就绪");
    return ESP_OK;
}
