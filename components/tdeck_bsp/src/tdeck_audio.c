// tdeck_audio.c — 音频输出(I2S 直推 class-D 功放)
//
// T-Deck 的喇叭侧【没有 codec 芯片】,ESP32-S3 的 I2S 直接推功放,
// 所以这里不需要任何 I2C 配置,只要把 I2S 时序摆对。
//
// 两个非显然的选择:
//
// 1. 用 MONO + SLOT_BOTH。功放的声道选择是板上电阻分压焊死的,软件读不到
//    它到底选了左还是右。两个 slot 送同一份数据,无论它取哪个都能出声。
//
// 2. 16bit 数据宽度(BCLK = 32 x LRCLK)。MAX98357A 这类功放的标准配置。
//    若板上是别的型号可能要 32bit —— 症状是【完全没声或刺耳噪声】,
//    而不是"声音小",据此可以快速判断。
//
// 麦克风(ES7210)不在这里 —— 它要经 I2C 配置,等小智集成时和它的
// codec 层一起做,避免两套实现抢同一颗芯片。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "tdeck_audio";

static i2s_chan_handle_t s_tx;
static uint32_t          s_rate;
static uint8_t           s_volume = 70;

esp_err_t tdeck_audio_init(uint32_t sample_rate)
{
    if (s_tx) return ESP_OK;
    s_rate = sample_rate;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;   // 没数据时自动填 0,否则停止播放会拖出一段噪声
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "I2S 通道创建失败");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,          // 功放不要 MCLK
            .bclk = TDECK_PIN_I2S_BCK,
            .ws   = TDECK_PIN_I2S_WS,
            .dout = TDECK_PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;   // 见文件头注释 1

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "I2S 标准模式配置失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S 使能失败");

    ESP_LOGI(TAG, "音频输出就绪 %" PRIu32 "Hz 单声道  BCK=%d WS=%d DOUT=%d",
             sample_rate, TDECK_PIN_I2S_BCK, TDECK_PIN_I2S_WS, TDECK_PIN_I2S_DOUT);
    return ESP_OK;
}

void tdeck_speaker_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
}

int tdeck_speaker_write(const int16_t* buf, size_t samples, uint32_t timeout_ms)
{
    if (!s_tx || !buf || !samples) return -1;

    // 音量在软件里做:功放没有音量控制脚,也没有 codec 可以调增益。
    // 用平方曲线而不是线性 —— 人耳对响度的感知接近对数,
    // 线性衰减的话"50%"听起来还是很吵,到 20% 才开始明显变小。
    int32_t gain = (int32_t)s_volume * s_volume;   // 0..10000
    static int16_t scaled[512];
    size_t done = 0;

    while (done < samples) {
        size_t n = samples - done;
        if (n > sizeof(scaled) / sizeof(scaled[0])) n = sizeof(scaled) / sizeof(scaled[0]);
        for (size_t i = 0; i < n; i++) {
            scaled[i] = (int16_t)((int32_t)buf[done + i] * gain / 10000);
        }
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx, scaled, n * sizeof(int16_t), &written,
                                        pdMS_TO_TICKS(timeout_ms));
        if (e != ESP_OK) return (int)(done * sizeof(int16_t));
        done += written / sizeof(int16_t);
    }
    return (int)(done * sizeof(int16_t));
}

int tdeck_mic_read(int16_t* buf, size_t samples, uint32_t timeout_ms)
{
    // 麦克风走 ES7210,需要 I2C 配置。等小智集成时一起做 ——
    // 现在返回 -1 而不是假装成功,免得调用方以为读到了静音。
    (void)buf; (void)samples; (void)timeout_ms;
    return -1;
}
