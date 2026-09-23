// tdeck_display.c — ST7789 320x240 + 背光
//
// 两个容易翻车的地方:
//
// 1. SPI 总线是三个从设备共享的(屏幕 CS=12 / SD 卡 CS=39 / SX1262 CS=9)。
//    初始化屏幕之前必须把另外两个 CS 显式拉高,否则它们会跟着响应时钟,
//    表现为屏幕花屏或干脆不亮 —— 而且这个现象很像"接线错了",很难往总线冲突上想。
//
// 2. T-Deck 的 ST7789 没有独立的 RST 引脚(官方 utilities.h 里就没有),
//    复位靠外设总电源。所以 reset_gpio_num 传 -1,靠 tdeck_power_on() 那一下上电复位。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

static const char* TAG = "tdeck_display";

#define TDECK_LCD_SPI_HOST      SPI2_HOST
#define TDECK_LCD_PIXEL_CLOCK   (40 * 1000 * 1000)   // 40MHz，ST7789 稳定跑得动
#define TDECK_LCD_CMD_BITS      8
#define TDECK_LCD_PARAM_BITS    8

#define TDECK_BL_LEDC_TIMER     LEDC_TIMER_0
#define TDECK_BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define TDECK_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define TDECK_BL_LEDC_RES       LEDC_TIMER_10_BIT     // 0..1023
#define TDECK_BL_LEDC_FREQ      5000

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static uint8_t                   s_backlight_percent;

// 把共享 SPI 总线上另外两个从设备的 CS 拉高，避免总线冲突（见文件头注释 1）
static void park_shared_spi_slaves(void)
{
    const gpio_num_t others[] = { TDECK_PIN_SDCARD_CS, TDECK_PIN_LORA_CS };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << others[i],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(others[i], 1);   // 高 = 未选中
    }
    ESP_LOGI(TAG, "已停放共享 SPI 上的 SD 卡(CS=%d)和 LoRa(CS=%d)",
             TDECK_PIN_SDCARD_CS, TDECK_PIN_LORA_CS);
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = TDECK_BL_LEDC_MODE,
        .timer_num       = TDECK_BL_LEDC_TIMER,
        .duty_resolution = TDECK_BL_LEDC_RES,
        .freq_hz         = TDECK_BL_LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "背光定时器配置失败");

    ledc_channel_config_t ch = {
        .gpio_num   = TDECK_PIN_LCD_BL,
        .speed_mode = TDECK_BL_LEDC_MODE,
        .channel    = TDECK_BL_LEDC_CHANNEL,
        .timer_sel  = TDECK_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "背光通道配置失败");
    return ESP_OK;
}

void tdeck_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_backlight_percent = percent;
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(TDECK_BL_LEDC_MODE, TDECK_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(TDECK_BL_LEDC_MODE, TDECK_BL_LEDC_CHANNEL);
}

uint8_t tdeck_backlight_get(void) { return s_backlight_percent; }

esp_lcd_panel_handle_t    tdeck_get_panel(void)    { return s_panel; }
esp_lcd_panel_io_handle_t tdeck_get_panel_io(void) { return s_panel_io; }

esp_err_t tdeck_display_init(esp_lcd_panel_handle_t* out_panel,
                             esp_lcd_panel_io_handle_t* out_io)
{
    park_shared_spi_slaves();

    spi_bus_config_t bus = {
        .mosi_io_num     = TDECK_PIN_SPI_MOSI,
        .miso_io_num     = TDECK_PIN_SPI_MISO,
        .sclk_io_num     = TDECK_PIN_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // 一次最多传一整屏(RGB565 = 2 字节/像素)
        .max_transfer_sz = TDECK_LCD_H_RES * TDECK_LCD_V_RES * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TDECK_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "SPI 总线初始化失败");

    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num       = TDECK_PIN_LCD_CS,
        .dc_gpio_num       = TDECK_PIN_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = TDECK_LCD_PIXEL_CLOCK,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = TDECK_LCD_CMD_BITS,
        .lcd_param_bits    = TDECK_LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TDECK_LCD_SPI_HOST, &io, &s_panel_io),
        TAG, "panel io 创建失败");

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,               // 见文件头注释 2：没有独立 RST
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &dev, &s_panel),
                        TAG, "ST7789 面板创建失败");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "面板复位失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "面板初始化失败");

    // ST7789 原生是 240x320 竖屏，T-Deck 横着用，所以交换 XY 再镜像。
    // ⚠️ swap/mirror 的组合要在真机上确认：镜像方向错了表现为画面左右翻转，
    //    只看代码看不出来。点亮后画一个非对称图形验证。
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true),        TAG, "swap_xy 失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false),  TAG, "mirror 失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),   TAG, "反色失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),    TAG, "开显示失败");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "背光初始化失败");

    ESP_LOGI(TAG, "ST7789 就绪 %dx%d @ %d MHz",
             TDECK_LCD_H_RES, TDECK_LCD_V_RES, TDECK_LCD_PIXEL_CLOCK / 1000000);

    if (out_panel) *out_panel = s_panel;
    if (out_io)    *out_io    = s_panel_io;
    return ESP_OK;
}
