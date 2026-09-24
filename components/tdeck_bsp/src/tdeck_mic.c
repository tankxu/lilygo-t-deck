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
#include <math.h>
#include <stdlib.h>

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
// ES7210 上哪一路接着 T-Deck 那颗麦克风,板子没有任何文档说。默认按最常见的
// MIC1+MIC2 来,探测不到声音时 tdeck_mic_probe_rms 会把四种组合都试一遍。
static uint8_t                       s_mic_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
static uint8_t                       s_slot;      // 取 I2S 的哪个 slot(0=左 1=右)
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
        .master_mode  = false,                // ESP 出时钟,ES7210 当从
        .mic_selected = s_mic_mask,           // 见文件头注释 3
        .mclk_src     = ES7210_MCLK_FROM_PAD,
        .mclk_div     = 0,                    // 0 = 用默认的 256
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
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(s_slot),
        .sample_rate     = s_rate,
        .mclk_multiple   = I2S_MCLK_MULTIPLE_256,
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "麦克风打开失败 %d", r);
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_channel_gain(s_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(s_slot), s_gain_db);
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
        esp_codec_dev_set_in_channel_gain(s_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(s_slot), db);
    }
}

bool tdeck_mic_running(void)
{
    return s_running;
}

// 换一种"哪颗 MIC / 哪个 slot"的组合。I2S 通道和 I2C 控制接口不动,
// 只重建 es7210 的 codec 对象 —— mic_selected 是 es7210_codec_new 时写进寄存器的,
// 改它必须重新 new 一次。
esp_err_t tdeck_mic_select(uint8_t mic_mask, uint8_t slot)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (mic_mask == s_mic_mask && slot == s_slot) return ESP_OK;

    bool was_running = s_running;
    if (s_running) tdeck_mic_stop();
    esp_codec_dev_delete(s_dev);
    audio_codec_delete_codec_if(s_codec_if);

    s_mic_mask = mic_mask;
    s_slot     = slot;

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if      = s_ctrl_if,
        .master_mode  = false,
        .mic_selected = s_mic_mask,
        .mclk_src     = ES7210_MCLK_FROM_PAD,
        .mclk_div     = 0,
    };
    s_codec_if = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "ES7210 重配失败");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec_dev 重建失败");

    if (was_running) tdeck_mic_start();
    return ESP_OK;
}

// 录一段算 RMS。内部用,不开关通道。
static int measure_rms(uint32_t ms, int* out_peak)
{
    const size_t N = 512;
    int16_t* buf = malloc(N * sizeof(int16_t));
    if (!buf) return -1;
    uint32_t frames = (ms * s_rate / 1000) / N;
    if (frames == 0) frames = 1;
    double acc = 0; size_t n = 0; int peak = 0;
    for (uint32_t f = 0; f < frames; f++) {
        if (tdeck_mic_read(buf, N, 500) <= 0) break;
        for (size_t i = 0; i < N; i++) {
            acc += (double)buf[i] * buf[i];
            int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        n += N;
    }
    free(buf);
    if (out_peak) *out_peak = peak;
    return n ? (int)sqrt(acc / n) : -1;
}

int tdeck_mic_probe_rms(uint32_t ms)
{
    // 上电自检:录一小段算 RMS。
    //
    // 为什么值得单独做一个:ES7210 有四路麦克风输入,T-Deck 只焊了一颗,
    // 但板子没说它接在哪一路。选错了的表现是【一切正常、只是永远静音】——
    // I2C 认得到芯片、I2S 收得到数据、上层跑得好好的,就是全 0。
    // 这条日志把"选对没有"从"跟服务器对完话才知道"提前到了开机 5 秒。
    //
    // 默认那组读不到东西时,把四种组合(MIC1/2 与 MIC3/4 x 左/右 slot)都试一遍,
    // 谁有信号就留谁 —— 省掉"改一行、编译、烧录、再听"的四轮来回。
    if (!s_dev) return -1;

    // 先确认 I2C 这一侧是通的:扫一遍总线,再读 ES7210 的芯片 ID(0xFD/0xFE = 0x72 0x10)。
    // 读不到就不用往下猜通道了 —— 问题在 I2C 或者外设电源,不在选通。
    // 扫描顺带验证扫描本身:键盘 0x55 和触摸 GT911 0x5D 一定在,
    // 它们都没出现就说明是总线/电源的事,跟 ES7210 无关。
    {
        i2c_master_bus_handle_t bus = tdeck_i2c_bus();
        char line[128]; int n = 0;
        line[0] = 0;
        for (uint8_t a = 0x08; a < 0x78 && n < 100; a++) {
            if (i2c_master_probe(bus, a, 50) == ESP_OK)   // 第三参是毫秒不是 tick
                n += snprintf(line + n, sizeof(line) - n, "0x%02X ", a);
        }
        ESP_LOGI(TAG, "I2C 总线上的器件:%s", n ? line : "(一个都没有)");
    }

    // ⚠️ ES7210 的 ID 寄存器是 0x3D/0x3E,不是 0xFD/0xFE ——
    // 后者是 ES8311 的习惯,读 ES7210 的未定义地址只会回 0xFF,
    // 看起来就像"芯片没应答",足以把排查方向整个带偏(我踩过)。
    int id_hi = 0, id_lo = 0;
    esp_codec_dev_read_reg(s_dev, 0x3D, &id_hi);
    esp_codec_dev_read_reg(s_dev, 0x3E, &id_lo);
    ESP_LOGI(TAG, "ES7210 芯片 ID = 0x%02X%02X(应为 0x7210)", id_hi & 0xFF, id_lo & 0xFF);

    // 再打几个关键寄存器:MIC12/34 的电源(0x4B/0x4C,0x00 才是开)、
    // 时钟分频(0x02)。全 0xFF 说明读不通,全 0x00 说明写进去了但没上电。
    int p12 = 0, p34 = 0, clk = 0;
    esp_codec_dev_read_reg(s_dev, 0x4B, &p12);
    esp_codec_dev_read_reg(s_dev, 0x4C, &p34);
    esp_codec_dev_read_reg(s_dev, 0x02, &clk);
    ESP_LOGI(TAG, "ES7210 寄存器:MIC12_PWR(0x4B)=0x%02X MIC34_PWR(0x4C)=0x%02X CLK(0x02)=0x%02X",
             p12 & 0xFF, p34 & 0xFF, clk & 0xFF);

    bool was_running = s_running;
    if (!was_running && tdeck_mic_start() != ESP_OK) return -1;

    int peak = 0;
    int rms = measure_rms(ms, &peak);
    ESP_LOGI(TAG, "录音自检:MIC%s slot%d → RMS=%d 峰值=%d",
             s_mic_mask == (ES7210_SEL_MIC1 | ES7210_SEL_MIC2) ? "1/2" : "3/4", s_slot, rms, peak);

    if (rms <= 0) {
        // 默认组合是哑的,把其它三种试一遍
        const struct { uint8_t mask; uint8_t slot; const char* name; } combos[] = {
            { ES7210_SEL_MIC1 | ES7210_SEL_MIC2, 1, "MIC1/2 slot1" },
            { ES7210_SEL_MIC3 | ES7210_SEL_MIC4, 0, "MIC3/4 slot0" },
            { ES7210_SEL_MIC3 | ES7210_SEL_MIC4, 1, "MIC3/4 slot1" },
        };
        for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
            if (tdeck_mic_select(combos[i].mask, combos[i].slot) != ESP_OK) continue;
            int p = 0, r = measure_rms(ms, &p);
            ESP_LOGI(TAG, "录音自检:%s → RMS=%d 峰值=%d", combos[i].name, r, p);
            if (r > 0) { rms = r; peak = p; break; }
        }
    }

    if (rms <= 0) {
        ESP_LOGW(TAG, "四种通道组合全是 0 —— 不是选通问题,查 MCLK(GPIO48)、"
                      "ES7210 供电,或者这块板子的麦克风根本没焊");
    } else {
        ESP_LOGI(TAG, "麦克风可用:MIC%s slot%d",
                 s_mic_mask == (ES7210_SEL_MIC1 | ES7210_SEL_MIC2) ? "1/2" : "3/4", s_slot);
    }

    if (!was_running) tdeck_mic_stop();
    return rms;
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
