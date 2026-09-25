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
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "tdeck_display";

#define TDECK_LCD_SPI_HOST      SPI2_HOST
// ⚠️ 这个值直接决定滑动的帧率上限。
// 整屏 320x240 RGB565 = 153600 字节,40MHz 下光传输就要 30.7ms,
// 也就是不管渲染多快,整屏重画都过不了 ~32 FPS。
// 换上 Xtensa 汇编把渲染压到 22ms 之后,SPI 就成了新的瓶颈。
//
// 一开始我看 LVGL 性能监视器里"刷屏只占 5ms"就排除了 SPI —— 那是误读:
// 那 5ms 只是排 DMA 的【CPU 时间】,真正的传输在后台异步跑,不计在里面。
#define TDECK_LCD_PIXEL_CLOCK   (80 * 1000 * 1000)   // 80MHz
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

// 诊断用:绕开 LVGL 直接往面板写,把「面板配置」和「LVGL 配置」两个嫌疑分开。
//
//   纯色铺不满 / 出条纹  → 面板这一侧的问题(行宽、字节序、色深)
//   纯色干净、方块位置错 → 面板没问题,方向参数要调
//   纯色干净、方块也对   → 面板完全正常,问题在 esp_lvgl_port
//
// 注意这里手工按【大端 RGB565】拼字节 —— ST7789 就是这么收的。
// 如果直写能出正确颜色,说明字节序判断没错。
esp_lcd_panel_handle_t    tdeck_get_panel(void)    { return s_panel; }
esp_lcd_panel_io_handle_t tdeck_get_panel_io(void) { return s_panel_io; }


// T-Deck 专用的电压与 gamma 调校。
//
// IDF 通用的 ST7789 驱动只发 SLPOUT / MADCTL / COLMOD / RAMCTRL,不碰电压和 gamma,
// 用出厂默认值。T-Deck 这块屏用默认值的表现是:饱和色正常,但接近黑的区域
// 压不到真黑,泛红并带渐变。
//
// 这组值来自 LilyGO 官方 TFT_eSPI 配置 Setup210_LilyGo_T_Deck.h 选中的
// INIT_SEQUENCE_2(注释就写着 "Different gamma values")。
// 其中 VCOMS(0xBB)=0x1A 是关键 —— VCOM 电压直接决定黑电平。
//
// 这里只发电压和 gamma 寄存器,不碰 MADCTL / COLMOD:那两个由 IDF 的 init
// 和后面的 swap_xy/mirror 负责,重复设置会打架。
static esp_err_t apply_tdeck_panel_tuning(esp_lcd_panel_io_handle_t io)
{
    struct { uint8_t cmd; uint8_t len; uint8_t data[14]; } seq[] = {
        { 0xB2, 5,  { 0x0C, 0x0C, 0x00, 0x33, 0x33 } },            // PORCTRL  前后廊
        { 0xB7, 1,  { 0x75 } },                                     // GCTRL    VGH/VGL
        { 0xBB, 1,  { 0x1A } },                                     // VCOMS    ← 黑电平
        { 0xC0, 1,  { 0x2C } },                                     // LCMCTRL
        { 0xC2, 1,  { 0x01 } },                                     // VDVVRHEN
        { 0xC3, 1,  { 0x13 } },                                     // VRHS
        { 0xC4, 1,  { 0x20 } },                                     // VDVSET
        { 0xC6, 1,  { 0x0F } },                                     // FRCTR2   帧率
        { 0xD0, 2,  { 0xA4, 0xA1 } },                               // PWCTRL1
        { 0xE0, 14, { 0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,     // PVGAMCTRL 正极性 gamma
                      0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30 } },
        { 0xE1, 14, { 0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,     // NVGAMCTRL 负极性 gamma
                      0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x3E } },
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, seq[i].cmd, seq[i].data, seq[i].len),
                            TAG, "发送 0x%02X 失败", seq[i].cmd);
    }
    ESP_LOGI(TAG, "已应用 T-Deck 电压/gamma 调校(%d 条)", (int)(sizeof(seq) / sizeof(seq[0])));
    return ESP_OK;
}

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
    ESP_RETURN_ON_ERROR(apply_tdeck_panel_tuning(s_panel_io), TAG, "面板调校失败");

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
