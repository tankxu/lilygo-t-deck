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
#include <driver/gpio.h>
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
// 官方 Microphone 例程给 MIC1|MIC2 设 0dB、给 MIC3|MIC4 设 **37.5dB(最大)** ——
// 把最大增益放哪一组,基本就说明话筒焊在哪一组上。所以默认选 MIC3/4。
// ⚠️ 必须【四颗全选】,不能只选 MIC3/4。
//
// ES7210 有【两个】数据输出脚。LilyGO 驱动里那行注释说得很清楚:
//     /* Force ADC1/2 output to SDOUT1 and ADC3/4 output to SDOUT2 */
// 而 T-Deck 只把其中一个 SDOUT 接到了 GPIO14。所以非 TDM 模式下只选
// MIC3/4 的话,数据全走到没接线的那个脚上 —— 实测 GPIO14 一次都不翻转、
// 恒为低,而 MCLK/SCK/LRCK 三根时钟都正常在动。
//
// 四颗全选会让 esp_codec_dev 打开 TDM(mic_num >= 3),四路时分复用到
// 同一个 SDOUT 上,连着的那根线才有数据。官方例程用的就是
// AUDIO_HAL_ADC_INPUT_ALL,正是这个原因。
//
// 配合 tdeck_mic_start 里的 channel=2 / channel_mask=0,esp_codec_dev 会走
// 它的 "Use 2 channel to fetch TDM data" 分支,把位宽减半塞进两个 slot。
static uint8_t                       s_mic_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                                                  ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
static uint8_t                       s_slot;      // 取 I2S 的哪个 slot(0=左 1=右)
// 模拟前端增益(dB)。30dB 是 esp-box / korvo 上验证过的驻极体麦克风起点。
static float                         s_gain_db = 37.5f;   // 官方例程给 MIC3/4 的值

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

    // I2C 扫描放在这里,因为 tdeck_mic_init 是【必经之路】——
    // 之前放在 probe 里,probe 又只在 codec 构造时调一次,日志里经常看不到。
    // 扫描顺带验证扫描本身:键盘 0x55 和触摸 GT911 0x5D 一定在,
    // 它们都没出现就说明是总线/电源的事,跟 ES7210 无关。
    {
        char line[128]; int n = 0; line[0] = 0;
        for (uint8_t a = 0x08; a < 0x78 && n < 100; a++) {
            if (i2c_master_probe(tdeck_i2c_bus(), a, 50) == ESP_OK)   // 第三参是毫秒不是 tick
                n += snprintf(line + n, sizeof(line) - n, "0x%02X ", a);
        }
        ESP_LOGI(TAG, "I2C 总线上的器件:%s(ES7210 应在 0x40)", n ? line : "(一个都没有)");
    }

    ESP_LOGI(TAG, "麦克风就绪 ES7210 %" PRIu32 "Hz  MCLK=%d SCK=%d LRCK=%d DIN=%d",
             sample_rate, TDECK_PIN_ES7210_MCLK, TDECK_PIN_ES7210_SCK,
             TDECK_PIN_ES7210_LRCK, TDECK_PIN_ES7210_DIN);
    return ESP_OK;
}

esp_err_t tdeck_mic_start(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;

    // ⚠️ channel_mask 【留 0】,不要指定单路。
    //
    // 传了 channel_mask,esp_codec_dev 会把 I2S rx 通道 reconfig 成
    // MONO + 单 slot,想让硬件直接丢掉另一路。看着省事,实际读回来【全是 0】。
    // 官方例程(examples/Microphone)也不这么干 —— 它老老实实收两个 slot
    // (I2S_CHANNEL_FMT_ALL_LEFT),在软件侧取需要的那一路。
    // 硬件已证明是好的(官方出厂自检里对着说话 VAD 计数能跳到四十几),
    // 所以差别只能出在这类配置上。
    //
    // 代价是 CPU 要做一次去交织,在 tdeck_mic_read 里做。
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .channel_mask    = 0,
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

    // 第一次开起来时把关键寄存器打一遍。放在 open 之后是因为 esp_codec_dev
    // 要设备处于打开状态才转发寄存器读写。
    //
    // ⚠️ ES7210 的 ID 寄存器是 0x3D/0x3E,不是 0xFD/0xFE —— 后者是 ES8311 的习惯,
    // 读 ES7210 的未定义地址永远回 0xFF,看起来像"芯片没应答",足以把排查
    // 方向整个带偏(已经带偏过一次)。
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        int id_hi = 0, id_lo = 0, p12 = 0, p34 = 0, clk = 0;
        esp_codec_dev_read_reg(s_dev, 0x3D, &id_hi);
        esp_codec_dev_read_reg(s_dev, 0x3E, &id_lo);
        esp_codec_dev_read_reg(s_dev, 0x4B, &p12);   // MIC12 电源,0x00 才是开
        esp_codec_dev_read_reg(s_dev, 0x4C, &p34);   // MIC34 电源
        esp_codec_dev_read_reg(s_dev, 0x02, &clk);   // 主时钟分频
        ESP_LOGI(TAG, "ES7210 ID=0x%02X%02X(应 0x7210) MIC12_PWR=0x%02X MIC34_PWR=0x%02X CLK=0x%02X",
                 id_hi & 0xFF, id_lo & 0xFF, p12 & 0xFF, p34 & 0xFF, clk & 0xFF);
        ESP_LOGI(TAG, "  判读:全 0xFF = I2C 读不通;全 0x00 = 配置进去了但通道没上电;"
                      "值正常 = 问题在 I2S 侧,查 MCLK(GPIO%d)有没有波形", TDECK_PIN_ES7210_MCLK);
    }
    // ── 对齐 LilyGO 官方 Microphone 例程 ──────────────────────
    //
    // 官方例程(examples/Microphone + lib/es7210)启动时把每颗 MIC 的
    // 模拟电源寄存器写成 0x00:
    //     REG47/48/49/4A = 0x00
    // 而 esp_codec_dev 的 es7210 驱动在 start 里写的是 **0x08**(bit3 置位)。
    // 这一位按数据手册是 PGA 的下电位 —— 置着的话 ADC 在跑、时钟也对,
    // 但模拟前端没工作,I2S 上读回来就是一片 0(而且 peak 也是 0,
    // 这正是我们看到的现象:活着的 ADC 就算在安静房间也会有几个 LSB 的底噪)。
    //
    // 另外官方 init 把 ANALOG_REG40 写成 0xC3,esp_codec_dev 写 0x43。
    //
    // 这些都在 esp_codec_dev 打开设备【之后】覆写,不然会被它的 start 覆盖回去。
    for (int r = 0x47; r <= 0x4A; r++) esp_codec_dev_write_reg(s_dev, r, 0x00);
    esp_codec_dev_write_reg(s_dev, 0x40, 0xC3);

    static bool dumped2 = false;
    if (!dumped2) {
        dumped2 = true;
        int m1 = 0, m2 = 0, m3 = 0, m4 = 0, an = 0, p12 = 0, p34 = 0;
        esp_codec_dev_read_reg(s_dev, 0x47, &m1);
        esp_codec_dev_read_reg(s_dev, 0x48, &m2);
        esp_codec_dev_read_reg(s_dev, 0x49, &m3);
        esp_codec_dev_read_reg(s_dev, 0x4A, &m4);
        esp_codec_dev_read_reg(s_dev, 0x40, &an);
        esp_codec_dev_read_reg(s_dev, 0x4B, &p12);
        esp_codec_dev_read_reg(s_dev, 0x4C, &p34);
        ESP_LOGI(TAG, "覆写后:MIC1..4_PWR=%02X %02X %02X %02X  ANALOG=%02X  "
                      "MIC12_PWR=%02X MIC34_PWR=%02X(0x00 才是开)",
                 m1 & 0xFF, m2 & 0xFF, m3 & 0xFF, m4 & 0xFF, an & 0xFF,
                 p12 & 0xFF, p34 & 0xFF);
    }

    ESP_LOGI(TAG, "开始采集 %" PRIu32 "Hz,增益 %.0fdB,MIC%s", s_rate, s_gain_db,
             s_mic_mask == (ES7210_SEL_MIC1 | ES7210_SEL_MIC2) ? "1/2" : "3/4");
    return ESP_OK;
}

// 用软件代替示波器:直接采 I2S 四根线的电平,数它翻转了多少次。
//
// 引脚虽然通过 GPIO matrix 接到了 I2S 外设,gpio_get_level() 读的仍然是
// 【焊盘】上的真实电平,所以不用表笔也能判断每根线到底有没有在动。
// 采样频率大概 1MHz 量级,远低于 MCLK 的 4.096MHz —— 数不准频率,
// 但"有没有翻转"是准的,而这正是我们要回答的问题。
//
// 判读:
//   MCLK/SCK/LRCK 不翻转 → ESP 这边时钟没出来(我们是主机,该查 i2s 配置)
//   时钟都在翻、DIN 不翻   → ES7210 没在 SDOUT 上驱动数据
//   DIN 也在翻但读回来全 0 → 数据到了外设却没进 DMA,问题在驱动层
void tdeck_mic_probe_pins(void)
{
    const int pins[4]    = { TDECK_PIN_ES7210_MCLK, TDECK_PIN_ES7210_SCK,
                             TDECK_PIN_ES7210_LRCK, TDECK_PIN_ES7210_DIN };
    const char* names[4] = { "MCLK", "SCK ", "LRCK", "DIN " };
    int flips[4] = {0}, high[4] = {0};
    const int N = 8000;

    // ⚠️ 先把输入通路打开再采。MCLK/SCK/LRCK 是【纯输出】,通过 GPIO matrix
    // 接到 I2S 外设时,IO_MUX 的输入使能位可能是关的 —— 那样 gpio_get_level()
    // 恒返回 0,一根好好在翻转的线会被误判成"死的"。
    // (DIN 本来就是输入,它的读数一直可信。)
    for (int k = 0; k < 4; k++) gpio_input_enable((gpio_num_t)pins[k]);

    // 四根线【先全采完再打日志】。之前边采边打,只有第二行出得来 ——
    // 关中断的紧循环和 UART 日志混在一起会丢输出。
    for (int k = 0; k < 4; k++) {
        // 【不要】关中断。关中断的紧循环夹着 USB-Serial-JTAG 的日志输出,
        // 会把后面的 ESP_LOG 整段吞掉(实测四行只出得来一行)。
        // 检测"有没有翻转"不需要连续采样,丢几个点无所谓。
        int last = gpio_get_level(pins[k]);
        for (int i = 0; i < N; i++) {
            int v = gpio_get_level(pins[k]);
            if (v != last) { flips[k]++; last = v; }
            high[k] += v;
        }
    }

    for (int k = 0; k < 4; k++) {
        ESP_LOGW(TAG, "  GPIO%-2d %s 翻转 %5d / %d,高电平 %2d%%  → %s",
                 pins[k], names[k], flips[k], N, high[k] * 100 / N,
                 flips[k] > 10 ? "在动" : "不动");
    }
    ESP_LOGW(TAG, "  判读:时钟三根都在动而 DIN 不动 = ES7210 没输出数据;"
                  "LRCK 不动 = 帧同步没发出去,ES7210 永远等不到一帧的开头");
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
        ESP_LOGW(TAG, "四种通道组合全是 0。直接采四根线的电平,看断在哪一段:");
        tdeck_mic_probe_pins();
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

    // 线上是两个 slot 交织的(见 tdeck_mic_start 里为什么不让硬件做单路),
    // 这里取 s_slot 那一路,按单声道交给调用方 —— 上层不用知道这件事。
    // 只有一个读者(小智的音频任务或自检),所以 scratch 放 static 不加锁;
    // 放栈上的话调用者要多背 2KB,而这个项目已经因为估错栈崩过两次。
    enum { FRAMES = 512 };
    static int16_t scratch[FRAMES * 2];

    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > FRAMES) n = FRAMES;
        int r = esp_codec_dev_read(s_dev, (void*)scratch, (int)(n * 2 * sizeof(int16_t)));
        if (r != ESP_CODEC_DEV_OK) return done ? (int)(done * sizeof(int16_t)) : -1;
        for (size_t i = 0; i < n; i++) buf[done + i] = scratch[i * 2 + (s_slot & 1)];
        done += n;
    }
    return (int)(done * sizeof(int16_t));
}
