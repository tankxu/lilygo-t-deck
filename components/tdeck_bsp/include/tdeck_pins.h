// tdeck_pins.h — LilyGO T-Deck 引脚定义
//
// 来源:Xinyuan-LilyGO/T-Deck examples/UnitTest/utilities.h
// 命名做了归类重排,数值与官方一致。详见 docs/hardware.md。

#pragma once

// ── 外设总电源 ────────────────────────────────────────────
// ⚠️ 必须最先拉高。不拉高则屏幕、I2C、音频全部不工作 —— T-Deck 第一大坑。
#define TDECK_PIN_POWERON        10

// ── SPI 共享总线(屏幕 / SD 卡 / LoRa 三个从设备靠 CS 区分)──
#define TDECK_PIN_SPI_MOSI       41
#define TDECK_PIN_SPI_MISO       38
#define TDECK_PIN_SPI_SCK        40

// ── 显示 ST7789 320x240 ──────────────────────────────────
#define TDECK_PIN_LCD_CS         12
#define TDECK_PIN_LCD_DC         11
#define TDECK_PIN_LCD_BL         42
#define TDECK_LCD_H_RES          320
#define TDECK_LCD_V_RES          240

// ── I2C 共享总线(键盘 0x55 / 触摸 GT911 / 音频 ADC ES7210)──
#define TDECK_PIN_I2C_SDA        18
#define TDECK_PIN_I2C_SCL        8
#define TDECK_I2C_ADDR_KEYBOARD  0x55

// ── 音频输出:I2S 直推 class-D 功放,板上无输出 codec ──────
#define TDECK_PIN_I2S_WS         5
#define TDECK_PIN_I2S_BCK        7
#define TDECK_PIN_I2S_DOUT       6

// ── 音频输入:ES7210 四通道 ADC(I2C 配置 + 独立 I2S)──────
#define TDECK_PIN_ES7210_MCLK    48
#define TDECK_PIN_ES7210_LRCK    21
#define TDECK_PIN_ES7210_SCK     47
#define TDECK_PIN_ES7210_DIN     14

// ── 输入设备 ──────────────────────────────────────────────
// 轨迹球四方向(官方命名 TBOX_G01..G04)
#define TDECK_PIN_TRACKBALL_UP    3
#define TDECK_PIN_TRACKBALL_DOWN  2
#define TDECK_PIN_TRACKBALL_LEFT  15
#define TDECK_PIN_TRACKBALL_RIGHT 1
// 轨迹球中键 = BOOT。启动后就是普通带上拉 GPIO,运行期复用作全局语音键(ADR-004)。
#define TDECK_PIN_TRACKBALL_CLICK 0

#define TDECK_PIN_TOUCH_INT      16
#define TDECK_PIN_KEYBOARD_INT   46

// ── 其它 ──────────────────────────────────────────────────
#define TDECK_PIN_SDCARD_CS      39
#define TDECK_PIN_BAT_ADC        4

#define TDECK_PIN_LORA_CS         9
#define TDECK_PIN_LORA_BUSY      13
#define TDECK_PIN_LORA_RST       17
#define TDECK_PIN_LORA_DIO1      45

// 仅 T-Deck Plus
#define TDECK_PIN_GPS_TX         43
#define TDECK_PIN_GPS_RX         44
