// tdeck_board.cc — 小智内核要的 Board 实现
//
// 上游的 Board 承担两件事:①初始化整块板子 ②给内核提供设备能力。
// 在这个工程里 ① 已经由 launcher 的 tdeck_bsp 做完了(ADR-002),
// 所以这里【只做 ②】,是一层不碰硬件的适配。
//
// ⚠️ 这一点是本文件最重要的约束:小智内核里任何"再初始化一遍"的代码
// 都必须被挡在门外。重复初始化的后果全是静默的:
//   · i2c_new_master_bus 同一个 port → ESP_ERR_INVALID_STATE(套着 ESP_ERROR_CHECK 直接重启)
//   · spi_bus_initialize 同一个 host → 同上
//   · ledc_timer_config 同一个 TIMER_0 → 不报错,但背光亮度被另一方改掉
//   · lv_init / lvgl_port_init → 不报错,现象是画面乱掉
// 所以这里不继承 WifiBoard(它会自己起 WifiManager 和配网热点),
// 直接继承 Board,联网状态从 OS 已经建好的 esp_event 默认循环里【旁听】。

#include "backlight.h"
#include "board.h"
#include "display.h"
#include "audio_codec.h"
#include "system_info.h"
#include "tdeck_audio_codec.h"
#include "xz_display.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_network.h>
#include <esp_wifi.h>
#include <font_awesome.h>
#include <cJSON.h>
#include <cstring>

// tdeck_bsp.h 自己带 extern "C" 守卫,这里【不能】再包一层 ——
// 它 include 的 esp_lcd 头文件在 C++ 下有重载声明,被硬塞进 extern "C"
// 就会报 conflicting declaration of C function。
#include "tdeck_bsp.h"

#define TAG "TdeckBoard"

// 小智的音频格式。输入 16k 是 Opus 编码器和 AFE 的既定值;
// 输出 24k 是服务端 TTS 的原生采样率 —— 写成别的值只会多一次重采样。
static constexpr int kInputSampleRate  = 16000;
static constexpr int kOutputSampleRate = 24000;

// Display 单例。放在文件作用域是因为 xiaozhi_core.cc 要拿它挂回调
// (Board::GetDisplay 返回的是基类指针,取不到 SetCallbacks)。
static XzDisplay s_display;
XzDisplay* tdeck_xz_display() { return &s_display; }

// 背光。只实现"怎么把亮度写下去",渐变和 NVS 由基类做。
// 注意【不要】用上游的 PwmBacklight:它会自己 ledc_timer_config(TIMER_0),
// 而 BSP 的背光已经占着 TIMER_0/CHANNEL_0 —— 重复配置不报错,
// 现象是两边互相改亮度,查起来很费劲(这是 T-Deck 上最隐蔽的一条重复初始化)。
class TdeckBacklight : public Backlight {
protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        tdeck_backlight_set(brightness);
    }
};

class TdeckBoard : public Board {
public:
    TdeckBoard() {
        ESP_LOGI(TAG, "小智 Board 适配层就绪(硬件已由 tdeck_bsp 初始化)");
    }

    std::string GetBoardType() override { return "wifi"; }

    AudioCodec* GetAudioCodec() override {
        static TdeckAudioCodec codec(kInputSampleRate, kOutputSampleRate);
        return &codec;
    }

    Display* GetDisplay() override { return &s_display; }

    // 给 MCP 的 self.screen.set_brightness 用 —— "小智,屏幕调暗点"是个很自然的请求。
    // ⚠️ 和音量一样:OS 那边(设置 app 的滑块)不知道亮度被改了,显示值会过期。
    Backlight* GetBacklight() override {
        static TdeckBacklight backlight;
        return &backlight;
    }

    NetworkInterface* GetNetwork() override {
        static EspNetwork network;
        return &network;
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        network_event_callback_ = callback;
    }

    // OS 的 net.cc 早就在连 WiFi 了(开机就连,不等小智)。
    // 这里【不发起连接】,只做两件事:挂上 esp_event 旁听后续的断连/重连,
    // 以及回答"现在是不是已经连上了"—— 后者不能省:
    // 小智通常是开机几十秒后才被唤起,那时 GOT_IP 事件早就过去了,
    // 只等事件的话内核会永远停在"等待网络"。
    void StartNetwork() override {
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &TdeckBoard::OnIpEvent, this, nullptr);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                            &TdeckBoard::OnWifiEvent, this, nullptr);
        if (IsConnected()) {
            Emit(NetworkEvent::Connected, Ssid());
        } else {
            Emit(NetworkEvent::Connecting, Ssid());
        }
    }

    const char* GetNetworkStateIcon() override {
        if (!IsConnected()) return FONT_AWESOME_WIFI_SLASH;
        int rssi = Rssi();
        if (rssi >= -65) return FONT_AWESOME_WIFI;
        if (rssi >= -75) return FONT_AWESOME_WIFI_FAIR;
        return FONT_AWESOME_WIFI_WEAK;
    }

    // 省电级别故意不接:WiFi 的 modem sleep 是整机共享的设置,
    // 小智一个 app 不该替 OS 决定。真要做也应该由 launcher 统一管。
    void SetPowerSaveLevel(PowerSaveLevel level) override { (void)level; }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        int pct = tdeck_battery_percent();
        if (pct < 0) return false;
        level = pct;
        // T-Deck 没有充电状态引脚,只能从"电压高于锂电上限"反推是不是插着电。
        charging    = tdeck_on_external_power();
        discharging = !charging;
        return true;
    }

    std::string GetBoardJson() override {
        std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
        json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
        if (IsConnected()) {
            json += R"("ssid":")" + Ssid() + R"(",)";
            json += R"("rssi":)" + std::to_string(Rssi()) + R"(,)";
            json += R"("ip":")" + Ip() + R"(",)";
        }
        json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
        return json;
    }

    // MCP 的 self.get_device_status 用这个回答"你现在什么情况"。
    std::string GetDeviceStatusJson() override {
        auto root = cJSON_CreateObject();

        auto speaker = cJSON_CreateObject();
        if (auto codec = GetAudioCodec()) {
            cJSON_AddNumberToObject(speaker, "volume", codec->output_volume());
        }
        cJSON_AddItemToObject(root, "audio_speaker", speaker);

        auto screen = cJSON_CreateObject();
        cJSON_AddNumberToObject(screen, "brightness", tdeck_backlight_get());
        cJSON_AddItemToObject(root, "screen", screen);

        int level = 0; bool charging = false, discharging = false;
        if (GetBatteryLevel(level, charging, discharging)) {
            auto battery = cJSON_CreateObject();
            cJSON_AddNumberToObject(battery, "level", level);
            cJSON_AddBoolToObject(battery, "charging", charging);
            cJSON_AddItemToObject(root, "battery", battery);
        }

        auto network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "type", "wifi");
        cJSON_AddStringToObject(network, "ssid", Ssid().c_str());
        int rssi = Rssi();
        cJSON_AddStringToObject(network, "signal",
                                rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak"));
        cJSON_AddItemToObject(root, "network", network);

        auto str = cJSON_PrintUnformatted(root);
        std::string result(str);
        cJSON_free(str);
        cJSON_Delete(root);
        return result;
    }

private:
    static void OnIpEvent(void* arg, esp_event_base_t, int32_t, void*) {
        auto* self = static_cast<TdeckBoard*>(arg);
        self->Emit(NetworkEvent::Connected, self->Ssid());
    }

    static void OnWifiEvent(void* arg, esp_event_base_t, int32_t, void*) {
        auto* self = static_cast<TdeckBoard*>(arg);
        self->Emit(NetworkEvent::Disconnected, "");
    }

    void Emit(NetworkEvent ev, const std::string& data) {
        if (network_event_callback_) network_event_callback_(ev, data);
    }

    static bool IsConnected() {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
    }

    static std::string Ssid() {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return "";
        return std::string(reinterpret_cast<const char*>(ap.ssid));
    }

    static int Rssi() {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -100;
        return ap.rssi;
    }

    static std::string Ip() {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK) return "";
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return std::string(buf);
    }

    NetworkEventCallback network_event_callback_ = nullptr;
};

DECLARE_BOARD(TdeckBoard);
