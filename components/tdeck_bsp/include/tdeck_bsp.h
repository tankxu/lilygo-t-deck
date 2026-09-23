// tdeck_bsp.h — T-Deck 板级支持包(ADR-002)
//
// 唯一与 T-Deck 硬件强绑定的一层。launcher 和每个 app 都只依赖这里的接口,
// 换板子只动这个 component。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_panel_ops.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 初始化 ────────────────────────────────────────────────
// 顺序是有讲究的:必须先给 GPIO10 上电并等外设稳定,再碰 I2C/SPI,
// 否则屏幕和 I2C 器件一个都认不到(T-Deck 第一大坑)。
esp_err_t tdeck_bsp_init(void);

// ── 电源 ──────────────────────────────────────────────────
esp_err_t tdeck_power_on(void);       // 拉高 GPIO10
esp_err_t tdeck_power_off(void);
int       tdeck_battery_mv(void);       // 电池电压,GPIO4 ADC,读不到返回 -1
int       tdeck_battery_percent(void);  // 0..100,查锂电放电曲线;读不到返回 -1
bool      tdeck_on_external_power(void); // 读数高于锂电上限 → 判定为外接供电

// ── 显示 ──────────────────────────────────────────────────
// 初始化 SPI 总线 + ST7789 + 背光。句柄交出去给 esp_lvgl_port 接管。
esp_err_t tdeck_display_init(esp_lcd_panel_handle_t* out_panel,
                             esp_lcd_panel_io_handle_t* out_io);

// 句柄 getter —— 供 esp_lvgl_port 接管
esp_lcd_panel_handle_t    tdeck_get_panel(void);
esp_lcd_panel_io_handle_t tdeck_get_panel_io(void);

void tdeck_backlight_set(uint8_t percent);   // 0..100,PWM on GPIO42
uint8_t tdeck_backlight_get(void);

// ── 输入 ──────────────────────────────────────────────────
// 键盘(I2C 0x55 的独立 ESP32-C3)、轨迹球(4 路方向 GPIO + 中键)、
// 触摸(GT911)都在这里归一化,最终喂给 LVGL 的 indev,
// 同时通过回调把原始事件抛给 app(自检应用要看原始事件)。

// 轨迹球事件的 code 取值
typedef enum {
    TDECK_TB_UP, TDECK_TB_DOWN, TDECK_TB_LEFT, TDECK_TB_RIGHT, TDECK_TB_CLICK,
} tdeck_trackball_dir_t;

typedef enum {
    TDECK_INPUT_KEYBOARD,
    TDECK_INPUT_TRACKBALL,
    TDECK_INPUT_TOUCH,
} tdeck_input_source_t;

typedef struct {
    tdeck_input_source_t source;
    int      code;        // 键盘:ASCII;轨迹球:方向枚举;触摸:未用
    bool     pressed;
    int16_t  x, y;        // 触摸坐标,其它来源为 0
    uint32_t timestamp_ms;
} tdeck_input_event_t;

esp_err_t tdeck_input_init(void);

// 键盘、触摸、ES7210 共用同一条 I2C 总线,句柄在这里取
i2c_master_bus_handle_t tdeck_i2c_bus(void);

// 触摸 GT911。返回 esp_lcd_touch 句柄交给 esp_lvgl_port 接管;没有触摸屏时返回 NULL。
esp_lcd_touch_handle_t tdeck_touch_init(void);

typedef void (*tdeck_input_cb_t)(const tdeck_input_event_t* ev, void* user);
void tdeck_input_subscribe(tdeck_input_cb_t cb, void* user);
void tdeck_input_unsubscribe(tdeck_input_cb_t cb);

// 轨迹球中键长按 —— 全局语音键(ADR-004)。无论当前哪个 app 在前台都会触发。
typedef void (*tdeck_voice_key_cb_t)(void* user);
void tdeck_set_voice_key_handler(tdeck_voice_key_cb_t cb, void* user);

// ── 音频 ──────────────────────────────────────────────────
// 输入:ES7210(I2C 配置 + 独立 I2S)。输出:I2S 直推 class-D 功放,无输出 codec。
esp_err_t tdeck_audio_init(uint32_t sample_rate);

// 阻塞读录音。返回实际读到的字节数,失败返回负数。
int tdeck_mic_read(int16_t* buf, size_t samples, uint32_t timeout_ms);

// 阻塞写播放。返回实际写出的字节数,失败返回负数。
int tdeck_speaker_write(const int16_t* buf, size_t samples, uint32_t timeout_ms);

void tdeck_speaker_set_volume(uint8_t percent);   // 0..100

#ifdef __cplusplus
}
#endif
