#include "tdeck_audio_codec.h"

#include <esp_log.h>

// tdeck_bsp.h 自己带 extern "C" 守卫,这里【不能】再包一层 ——
// 它 include 的 esp_lcd 头文件在 C++ 下有重载声明,被硬塞进 extern "C"
// 就会报 conflicting declaration of C function。
#include "tdeck_bsp.h"

#define TAG "TdeckAudioCodec"

TdeckAudioCodec::TdeckAudioCodec(int input_sample_rate, int output_sample_rate) {
    duplex_ = false;
    input_reference_ = false;
    input_channels_ = 1;
    output_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;   // ES7210 模拟前端增益(dB),驻极体麦克风的经验起点

    // 只建对象、配寄存器,不开始采集 —— 开始采集由 EnableInput 控制。
    // 麦克风常开会一直占着 I2S_NUM_1 的 DMA 和 ES7210 的模拟电流。
    tdeck_mic_init((uint32_t)input_sample_rate_);
    tdeck_mic_set_gain(input_gain_);

    // 录 150ms 自检。没有它的话,"麦克风到底通不通"要等到跟服务器对上话、
    // 而且对方一直听不见你说什么的时候才会被怀疑 —— 那时候可疑的东西太多了。
    tdeck_mic_probe_rms(150);
}

TdeckAudioCodec::~TdeckAudioCodec() {
    tdeck_mic_stop();
}

void TdeckAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) return;
    if (enable) {
        tdeck_mic_init((uint32_t)input_sample_rate_);   // 幂等
        if (tdeck_mic_start() != ESP_OK) {
            ESP_LOGE(TAG, "麦克风起不来,后面读到的会是静音");
            return;
        }
    } else {
        tdeck_mic_stop();
    }
    AudioCodec::EnableInput(enable);
}

void TdeckAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) return;
    if (enable) {
        // ⚠️ 喇叭是共享资源:音乐 app 也会用 tdeck_audio_init 按【它的】解码格式
        // 重建同一个 I2S 通道。这里在开播之前把格式抢回 24kHz 单声道,
        // 否则小智的 PCM 会按音乐上一次设的采样率放出去(听起来被拉长或加速)。
        // 目前不做仲裁 —— 谁最后说话谁说了算,两边同时出声的场面本来也没有意义。
        tdeck_audio_init((uint32_t)output_sample_rate_, 1);
    }
    // 没有功放使能脚(T-Deck 的功放跟着 GPIO10 总电源走),
    // 所以"关闭输出"这一侧没有硬件动作:BSP 的通道 auto_clear 会持续推 0。
    AudioCodec::EnableOutput(enable);
}

void TdeckAudioCodec::SetOutputVolume(int volume) {
    AudioCodec::SetOutputVolume(volume);   // 记进 NVS(小智自己的 audio 命名空间)
    // 功放没有音量脚,增益全在 BSP 的软件缩放里。这里直接下发,
    // 好处是 MCP 的"调音量"是真能听出来的。
    // ⚠️ 代价:OS 那边的系统音量(main/sys/volume.cc)不知道值被改了,
    // HUD 和它的 NVS 会和实际音量不一致。等 OS 暴露一个 C 接口再接过去。
    tdeck_speaker_set_volume((uint8_t)(volume < 0 ? 0 : (volume > 100 ? 100 : volume)));
}

int TdeckAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_) return 0;
    int bytes = tdeck_mic_read(dest, (size_t)samples, 1000);
    if (bytes <= 0) return 0;
    return bytes / (int)sizeof(int16_t);
}

int TdeckAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_) return samples;
    // 音量不在这里做:BSP 的 tdeck_speaker_write 已经按系统音量做了平方曲线缩放,
    // 这里再乘一次就是双重衰减(现象是"音量拉满也很小声")。
    int bytes = tdeck_speaker_write(data, (size_t)samples, 1000);
    if (bytes <= 0) return 0;
    return bytes / (int)sizeof(int16_t);
}
