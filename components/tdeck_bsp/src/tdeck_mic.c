// tdeck_mic.c — 麦克风输入(ES7210 四通道音频 ADC)
//
// 为什么麦克风单独一个文件、而且放在 BSP 而不是小智里(ADR-002):
// ES7210 是一颗需要 I2C 配置的芯片,上电后默认不出数据 —— 必须写寄存器选通
// MIC、设 MCLK/LRCK 分频系数、设模拟增益。谁先配它谁就是它的主人,
// 两套实现(比如小智一套、录音 app 一套)同时配同一颗芯片,后配的会把先配的
// 改掉,而且不报任何错。所以它和喇叭一样收进 BSP,上层只看到 read。
//
// 三个和喇叭那一路完全不同的地方:
//
// 1. 用 I2S_NUM_1。喇叭占着 I2S_NUM_0(tdeck_audio.c),而麦克风这一路的引脚
//    (MCLK=48 SCK=47 LRCK=21 DIN=14)和喇叭那一路(BCK=7 WS=5 DOUT=6)完全
//    不重叠 —— 物理上就不可能共用一个 I2S 外设,也就没有"全双工"这回事。
//
// 2. 必须给 MCLK。ES7210 的 ADC 内部时钟是从 MCLK 分出来的,不接 MCLK 芯片
//    能被 I2C 认到、寄存器能读能写,但 I2S 线上读回来全 0 —— 现象极像"接线错了"。
//
// 3. 只选通 MIC1 + MIC2 两路。esp_codec_dev 的 es7210 驱动用"选通 >= 3 路"
//    作为切 TDM 的判据,选两路才走普通 I2S(左右两个 slot)。T-Deck 只有一颗
//    麦克风,没必要为它拉 TDM —— 那会要求 rx 通道也按 TDM 初始化,多一层出错面。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <driver/i2s_std.h>
#include <esp_check.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>

static const char* TAG = "tdeck_mic";

// DMA 参数照抄小智 audio_codec.h 里的值:6 个描述符 x 240 帧,16kHz 下约 90ms
// 缓冲。再小会在 WiFi 收发抢总线时丢帧,再大是白占内部 RAM(DMA 缓冲不能进 PSRAM)。
#define MIC_DMA_DESC_NUM   6
#define MIC_DMA_FRAME_NUM  240

// ES7210 的 7 位 I2C 地址。esp_codec_dev 的宏 ES7210_CODEC_DEFAULT_ADDR 是
// 8 位写地址(0x80),两者差一个移位 —— 传错了表现为 I2C 探测得到、配置全失败。
#define ES7210_I2C_ADDR_8BIT  ES7210_CODEC_DEFAULT_ADDR

static i2s_chan_handle_t             s_rx;
static const audio_codec_data_if_t*  s_data_if;
static const audio_codec_ctrl_if_t*  s_ctrl_if;
static const audio_codec_if_t*       s_codec_if;
static esp_codec_dev_handle_t        s_dev;
static uint32_t                      s_rate;
static bool                          s_running;
// 模拟前端增益(dB)。30dB 是 esp-box / korvo 上验证过的驻极体麦克风起点。
static float                         s_gain_db = 30.0f;

esp_err_t tdeck_mic_init(uint32_t sample_rate)
{
    if (sample_rate == 0) sample_rate = 16000;
    if (s_dev && s_rate == sample_rate) return ESP_OK;   // 幂等
    if (s_dev) {
        // 采样率变了:通道不用重建,esp_codec_dev_open 会连 I2S 时钟一起 reconfig,
        // 所以只要记下新值,下一次 start 时生效。
        s_rate = sample_rate;
        return ESP_OK;
    }
    s_rate = sample_rate;

    i2s_chan_config_t chan = {
        .id                   = I2S_NUM_1,
        .role                 = I2S_ROLE_MASTER,
        .dma_desc_num         = MIC_DMA_DESC_NUM,
        .dma_frame_num        = MIC_DMA_FRAME_NUM,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority        = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx), TAG, "I2S RX 通道创建失败");

    // 这里的时钟和 slot 只是占位:真正生效的是 esp_codec_dev_open() 里按
    // esp_codec_dev_sample_info_t 做的 reconfig。必须在这里定死的只有两件事 ——
    // ① 通道工作在 STD 模式(reconfig 只能在同一 comm mode 内改)
    // ② GPIO 映射(reconfig 不碰 GPIO)
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            // ES7210 靠 MCLK/LRCK 的比值查内部分频表,默认 256。
            // 这个值必须和 open 时传的 mclk_multiple 一致。
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = TDECK_PIN_ES7210_MCLK,   // 见文件头注释 2:少了它读回来全 0
            .bclk = TDECK_PIN_ES7210_SCK,
            .ws   = TDECK_PIN_ES7210_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din  = TDECK_PIN_ES7210_DIN,
            .invert_flags = { false, false, false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "I2S RX 配置失败");

    // tx_handle 传 NULL 是有意的:喇叭那一路不归 esp_codec_dev 管。
    // 交给它的话,open 输入时它会顺带 reconfig 配对的 tx 通道时钟
    // (audio_codec_data_i2s.c 的 set_fs),把喇叭的采样率也改掉。
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_1,
        .rx_handle = s_rx,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "I2S data_if 创建失败");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_NUM_0,
        .addr       = ES7210_I2C_ADDR_8BIT,
        .bus_handle = tdeck_i2c_bus(),    // 复用 BSP 已建好的总线,不能再 new 一条
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if, ESP_FAIL, TAG, "ES7210 I2C ctrl_if 创建失败");

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if      = s_ctrl_if,
        .master_mode  = false,                                 // ESP 出时钟,ES7210 当从
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,     // 见文件头注释 3
        .mclk_src     = ES7210_MCLK_FROM_PAD,
        .mclk_div     = 0,                                     // 0 = 用默认的 256
    };
    s_codec_if = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "ES7210 初始化失败(检查 I2C 0x40)");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec_dev 创建失败");

    ESP_LOGI(TAG, "麦克风就绪 ES7210 %" PRIu32 "Hz  MCLK=%d SCK=%d LRCK=%d DIN=%d",
             sample_rate, TDECK_PIN_ES7210_MCLK, TDECK_PIN_ES7210_SCK,
             TDECK_PIN_ES7210_LRCK, TDECK_PIN_ES7210_DIN);
    return ESP_OK;
}

esp_err_t tdeck_mic_start(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;

    // channel = 2:非 TDM 的 ES7210 在 I2S 线上固定出两个 slot。
    // channel_mask 只留第 0 路 → esp_codec_dev 把 rx 通道 reconfig 成 MONO+SLOT_LEFT,
    // 由硬件丢掉右 slot,省掉 CPU 做去交织。
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate     = s_rate,
        .mclk_multiple   = I2S_MCLK_MULTIPLE_256,
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "麦克风打开失败 %d", r);
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_channel_gain(s_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), s_gain_db);
    s_running = true;
    ESP_LOGI(TAG, "开始采集 %" PRIu32 "Hz,增益 %.0fdB", s_rate, s_gain_db);
    return ESP_OK;
}

esp_err_t tdeck_mic_stop(void)
{
    if (!s_dev || !s_running) return ESP_OK;
    esp_codec_dev_close(s_dev);
    s_running = false;
    return ESP_OK;
}

void tdeck_mic_set_gain(float db)
{
    s_gain_db = db;
    if (s_running) {
        esp_codec_dev_set_in_channel_gain(s_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), db);
    }
}

bool tdeck_mic_running(void)
{
    return s_running;
}

int tdeck_mic_read(int16_t* buf, size_t samples, uint32_t timeout_ms)
{
    // timeout_ms 收下但用不上:esp_codec_dev_read 内部是阻塞的 i2s_channel_read
    // (portMAX_DELAY),没有超时参数。保留这个形参是为了和 tdeck_speaker_write
    // 对称,将来真需要超时再在这里补一层。
    (void)timeout_ms;
    if (!s_dev || !s_running || !buf || !samples) return -1;

    int r = esp_codec_dev_read(s_dev, (void*)buf, (int)(samples * sizeof(int16_t)));
    if (r != ESP_CODEC_DEV_OK) return -1;
    return (int)(samples * sizeof(int16_t));
}
