#ifndef _TDECK_AUDIO_CODEC_H
#define _TDECK_AUDIO_CODEC_H

#include "audio_codec.h"

// T-Deck 的 AudioCodec 实现 —— 一层【很薄】的适配器。
//
// 上游现成的类没有一个能直接用:
//
//  · BoxAudioCodec 假定收发挂在同一个 I2S 口、共用 MCLK/BCLK/LRCK,靠 ES8311 做 DAC。
//    T-Deck 没有输出 codec,而且喇叭那一路(WS=5 BCK=7 DOUT=6)和麦克风那一路
//    (MCLK=48 SCK=47 LRCK=21 DIN=14)引脚完全不重叠,物理上没法共用一个 I2S 外设。
//  · NoAudioCodecSimplex 两头都是裸 I2S,但 ES7210 是要 I2C 配置的芯片,
//    裸读只会读到全 0;而且它会自己 i2s_new_channel(I2S_NUM_0),
//    和 BSP 已经建好的喇叭通道打架。
//
// 所以这里【一行硬件代码都不写】,全部转发给 BSP(ADR-002:硬件只有 BSP 一个主人):
//    输入 → tdeck_mic_*      (ES7210 + I2S_NUM_1)
//    输出 → tdeck_speaker_*  (裸 I2S_NUM_0 + 系统软件音量)
//
// duplex_ 报 false:两个 I2S 口有各自独立的时钟,不是硬件双工。
// input_reference_ 也是 false:功放输出没有回接到 ES7210 任何一路,
// 拿不到回采参考信号 —— 设备侧 AEC 在这块板上做不了(只能用服务端 AEC)。
class TdeckAudioCodec : public AudioCodec {
public:
    TdeckAudioCodec(int input_sample_rate, int output_sample_rate);
    virtual ~TdeckAudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual void SetOutputVolume(int volume) override;

protected:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};

#endif // _TDECK_AUDIO_CODEC_H
