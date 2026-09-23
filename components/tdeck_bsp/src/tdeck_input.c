// tdeck_input.c — 键盘(I2C 0x55)+ 轨迹球(4 方向 GPIO + 中键)
//
// 两个器件的读法都以 LilyGO UnitTest.ino 为准:
//
//   键盘   独立的 ESP32-C3,I2C 从机地址 0x55。读 1 字节,非 0 即按下的 ASCII。
//          主控不用管矩阵扫描。
//
//   轨迹球 四个方向各一根 GPIO,内部上拉。滚动时对应引脚【电平翻转】——
//          上升沿下降沿都算一格,所以判据是"和上次读到的不一样",不是沿检测。
//          中键接 GPIO0(也就是 BOOT),按下为低。
//
// 统一轮询,20ms 一轮:轨迹球滚得再快也不会超过这个频率能捕捉的范围,
// 而键盘 I2C 读取本身有几百微秒开销,再快没意义。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "tdeck_input";

#define MAX_SUBS         4
#define POLL_PERIOD_MS   20
#define VOICE_HOLD_MS    600    // 中键长按多久算语音键(ADR-004)

static struct { tdeck_input_cb_t cb; void* user; } s_subs[MAX_SUBS];
static tdeck_voice_key_cb_t s_voice_cb;
static void*                s_voice_user;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_kbd;
static bool                    s_kbd_ok;

static void emit(tdeck_input_source_t src, int code, bool pressed)
{
    tdeck_input_event_t ev = {
        .source       = src,
        .code         = code,
        .pressed      = pressed,
        .x = 0, .y = 0,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].cb) s_subs[i].cb(&ev, s_subs[i].user);
    }
}

i2c_master_bus_handle_t tdeck_i2c_bus(void) { return s_bus; }

// 注入一个输入事件,走和真实硬件【完全相同】的分发路径。
// 调试接口用它模拟按键和轨迹球 —— 绕过分发直接给 app 发事件的话,
// 测出来的行为不等于真实行为,那种"测试"没有价值。
void tdeck_input_inject(tdeck_input_source_t src, int code)
{
    emit(src, code, true);
}

void tdeck_input_subscribe(tdeck_input_cb_t cb, void* user)
{
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].cb) { s_subs[i].cb = cb; s_subs[i].user = user; return; }
    }
    ESP_LOGW(TAG, "订阅者已满(%d)", MAX_SUBS);
}

void tdeck_input_unsubscribe(tdeck_input_cb_t cb)
{
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].cb == cb) { s_subs[i].cb = NULL; s_subs[i].user = NULL; }
    }
}

void tdeck_set_voice_key_handler(tdeck_voice_key_cb_t cb, void* user)
{
    s_voice_cb = cb; s_voice_user = user;
}

// ── 轨迹球 ────────────────────────────────────────────────
static const struct { gpio_num_t pin; tdeck_trackball_dir_t dir; } s_tb[] = {
    { TDECK_PIN_TRACKBALL_UP,    TDECK_TB_UP    },
    { TDECK_PIN_TRACKBALL_DOWN,  TDECK_TB_DOWN  },
    { TDECK_PIN_TRACKBALL_LEFT,  TDECK_TB_LEFT  },
    { TDECK_PIN_TRACKBALL_RIGHT, TDECK_TB_RIGHT },
};
static int s_tb_last[4];

static esp_err_t trackball_init(void)
{
    uint64_t mask = 1ULL << TDECK_PIN_TRACKBALL_CLICK;
    for (size_t i = 0; i < 4; i++) mask |= 1ULL << s_tb[i].pin;

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // 官方就是 INPUT_PULLUP
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      // 轮询,不用中断
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "轨迹球 GPIO 配置失败");

    for (size_t i = 0; i < 4; i++) s_tb_last[i] = gpio_get_level(s_tb[i].pin);
    return ESP_OK;
}

// ── 键盘 ──────────────────────────────────────────────────
static esp_err_t keyboard_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = TDECK_PIN_I2C_SDA,
        .scl_io_num        = TDECK_PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "I2C 总线创建失败");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TDECK_I2C_ADDR_KEYBOARD,
        .scl_speed_hz    = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev, &s_kbd), TAG, "键盘设备添加失败");

    // 键盘 MCU 自己也要启动时间。上电后立刻探测经常探不到,这里给它几次机会。
    for (int i = 0; i < 10; i++) {
        if (i2c_master_probe(s_bus, TDECK_I2C_ADDR_KEYBOARD, 100) == ESP_OK) {
            s_kbd_ok = true;
            ESP_LOGI(TAG, "键盘已就绪(I2C 0x%02X,第 %d 次探测)", TDECK_I2C_ADDR_KEYBOARD, i + 1);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "键盘未响应(I2C 0x%02X)—— 轨迹球仍可用", TDECK_I2C_ADDR_KEYBOARD);
    return ESP_OK;   // 键盘没接上不该让整个 BSP 起不来
}

// ── 轮询任务 ──────────────────────────────────────────────
static void input_task(void* arg)
{
    int  click_last  = 1;      // 上拉,松开为高
    int64_t click_at = 0;
    bool voice_fired = false;

    while (1) {
        // 轨迹球:电平和上次不同就是滚了一格
        for (size_t i = 0; i < 4; i++) {
            int lv = gpio_get_level(s_tb[i].pin);
            if (lv != s_tb_last[i]) {
                s_tb_last[i] = lv;
                emit(TDECK_INPUT_TRACKBALL, s_tb[i].dir, true);
            }
        }

        // 中键:短按 = 确认,长按 = 全局语音键(ADR-004)
        int click = gpio_get_level(TDECK_PIN_TRACKBALL_CLICK);
        if (click != click_last) {
            click_last = click;
            if (click == 0) {                       // 按下
                click_at = esp_timer_get_time();
                voice_fired = false;
            } else {                                // 松开
                if (!voice_fired) emit(TDECK_INPUT_TRACKBALL, TDECK_TB_CLICK, true);
            }
        } else if (click == 0 && !voice_fired &&
                   (esp_timer_get_time() - click_at) / 1000 >= VOICE_HOLD_MS) {
            // 长按够时间就立刻触发,不等松手 —— 语音键要的是"按住说话"的手感
            voice_fired = true;
            if (s_voice_cb) s_voice_cb(s_voice_user);
        }

        // 键盘:读 1 字节,非 0 即 ASCII
        if (s_kbd_ok) {
            uint8_t ch = 0;
            if (i2c_master_receive(s_kbd, &ch, 1, 50) == ESP_OK && ch != 0) {
                emit(TDECK_INPUT_KEYBOARD, ch, true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t tdeck_input_init(void)
{
    ESP_RETURN_ON_ERROR(trackball_init(), TAG, "轨迹球初始化失败");
    ESP_RETURN_ON_ERROR(keyboard_init(),  TAG, "键盘初始化失败");

    BaseType_t ok = xTaskCreate(input_task, "tdeck_input", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "输入任务创建失败");

    ESP_LOGI(TAG, "输入层就绪(轮询 %dms)", POLL_PERIOD_MS);
    return ESP_OK;
}
