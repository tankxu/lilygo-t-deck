// tdeck_power.c — 外设总电源 + 电池电压
//
// GPIO10 是全板外设的电源开关。不拉高的话屏幕、I2C(键盘/触摸/ES7210)
// 一个都认不到 —— 这是 T-Deck 移植踩的第一个坑,所以它必须是 init 的第一步。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "tdeck_power";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_adc_ready;

esp_err_t tdeck_power_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TDECK_PIN_POWERON,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "配置 GPIO%d 失败", TDECK_PIN_POWERON);
    ESP_RETURN_ON_ERROR(gpio_set_level(TDECK_PIN_POWERON, 1), TAG, "拉高失败");

    // 外设上电后需要时间稳定。经验值 100ms:太短的话紧接着的 I2C 扫描
    // 会扫不到键盘(0x55),表现为"键盘时有时无"。
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "外设电源已开(GPIO%d 拉高)", TDECK_PIN_POWERON);
    return ESP_OK;
}

esp_err_t tdeck_power_off(void)
{
    return gpio_set_level(TDECK_PIN_POWERON, 0);
}

// 电池走 GPIO4 → ESP32-S3 的 ADC1_CH3,板上是 1:2 分压,所以读数要 ×2。
#define TDECK_BAT_ADC_UNIT     ADC_UNIT_1
#define TDECK_BAT_ADC_CHANNEL  ADC_CHANNEL_3
#define TDECK_BAT_DIVIDER      2

static void battery_adc_init(void)
{
    if (s_adc_ready) return;

    adc_oneshot_unit_init_cfg_t unit = { .unit_id = TDECK_BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC 初始化失败,电池电压不可用");
        return;
    }

    adc_oneshot_chan_cfg_t chan = {
        .atten    = ADC_ATTEN_DB_12,   // 满量程约 3.1V,配合 1:2 分压覆盖单节锂电
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, TDECK_BAT_ADC_CHANNEL, &chan);

    // 有校准曲线就用,没有就回落到裸读数换算(精度差些但不至于没值)
    adc_cali_curve_fitting_config_t cali = {
        .unit_id  = TDECK_BAT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "ADC 无校准曲线,电压读数为估算值");
    }
    s_adc_ready = true;
}

int tdeck_battery_mv(void)
{
    battery_adc_init();
    if (!s_adc_ready) return -1;

    // 多采几次取平均。ADC 单次读数抖动能到几十 mV,电量百分比会跟着跳。
    const int N = 8;
    int sum = 0;
    for (int i = 0; i < N; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, TDECK_BAT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        sum += raw;
    }
    int raw_avg = sum / N;

    int mv = 0;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) != ESP_OK) return -1;
    } else {
        mv = raw_avg * 3100 / 4095;   // 12dB 衰减下满量程约 3.1V
    }
    return mv * TDECK_BAT_DIVIDER;
}
