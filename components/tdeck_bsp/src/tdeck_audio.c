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
// ⚠️ 采样率【必须】用解码器报的真实值,不能按源流的标称值写死。
// Opus 内部一律按 48kHz 工作,即使源流标称 24kHz,解码器也可能输出 48kHz;
// 声道数同理。写死 24kHz 单声道去播 48kHz 的 PCM,声音会慢一倍 ——
// 这种错不会报任何错误,只是听起来"被拉长了"。
//
// 麦克风(ES7210)不在这里 —— 它要经 I2C 配置,实现在 tdeck_mic.c,
// 用的是独立的 I2S_NUM_1(引脚和喇叭这一路完全不重叠,物理上没法共用外设)。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_check.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "tdeck_audio";

static i2s_chan_handle_t s_tx;
static uint32_t          s_rate;
static uint8_t           s_volume = 70;

static uint8_t s_ch = 1;

// 喇叭是【独占】资源:音乐 app 和小智都会往里写。下面那块缩放缓冲是共享的,
// 两个任务同时进来会互相把对方的样本覆盖掉,放出来是一片撕裂的杂音。
// 锁的粒度就是"一整块写完",谁拿到锁谁在这段时间里独占喇叭 ——
// 把两路 PCM 交织进同一条 I2S 流本来也不是能听的东西,混音是另一回事。
static SemaphoreHandle_t s_spk_mux;

esp_err_t tdeck_audio_init(uint32_t sample_rate, uint8_t channels)
{
    if (channels == 0) channels = 1;
    // 格式没变就不动。变了必须整个重建通道 —— i2s_channel_reconfig_std_clock
    // 只能改时钟,改不了声道模式,而声道数变了 slot 布局也要跟着变。
    if (s_tx && s_rate == sample_rate && s_ch == channels) return ESP_OK;
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_rate = sample_rate;
    s_ch   = channels;

    if (!s_spk_mux) {
        s_spk_mux = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_spk_mux, ESP_ERR_NO_MEM, TAG, "喇叭互斥量创建失败");
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;   // 没数据时自动填 0,否则停止播放会拖出一段噪声
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "I2S 通道创建失败");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                        channels >= 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,          // 功放不要 MCLK
            .bclk = TDECK_PIN_I2S_BCK,
            .ws   = TDECK_PIN_I2S_WS,
            .dout = TDECK_PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    if (channels < 2) std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;   // 见文件头注释 1

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "I2S 标准模式配置失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S 使能失败");

    ESP_LOGI(TAG, "音频输出就绪 %" PRIu32 "Hz %s  BCK=%d WS=%d DOUT=%d",
             sample_rate, channels >= 2 ? "立体声" : "单声道",
             TDECK_PIN_I2S_BCK, TDECK_PIN_I2S_WS, TDECK_PIN_I2S_DOUT);
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

    // scaled 是共享的,必须在锁里用。见文件上方 s_spk_mux 的说明。
    // 放 static 而不是栈上:这个函数的调用者(音乐解码任务、小智音频任务)
    // 栈都不宽裕,1KB 白拿走不划算 —— 这个项目已经因为估错栈崩过两次了。
    static int16_t scaled[512];
    size_t done = 0;

    if (s_spk_mux && xSemaphoreTake(s_spk_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;   // 别人正占着喇叭,这一块丢掉,总比跟他搅在一起强
    }

    while (done < samples) {
        size_t n = samples - done;
        if (n > sizeof(scaled) / sizeof(scaled[0])) n = sizeof(scaled) / sizeof(scaled[0]);
        for (size_t i = 0; i < n; i++) {
            scaled[i] = (int16_t)((int32_t)buf[done + i] * gain / 10000);
        }
        size_t written = 0;
        esp_err_t e = i2s_channel_write(s_tx, scaled, n * sizeof(int16_t), &written,
                                        pdMS_TO_TICKS(timeout_ms));
        if (e != ESP_OK) break;
        done += written / sizeof(int16_t);
    }

    if (s_spk_mux) xSemaphoreGive(s_spk_mux);
    return (int)(done * sizeof(int16_t));
}
