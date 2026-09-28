#pragma once
namespace tdeck {
// 启动联网:WiFi → SNTP 对时 → 定期取天气。全程异步,不阻塞 app_main。
void net_start();

// ── 给设置应用用的接口 ──
struct NetStatus {
    bool online;
    char ssid[33];
    char ip[16];
    int  rssi;
};
NetStatus net_status();

// 扫描周边 AP。返回条数,结果写进 out(按信号强度降序,已去重)。
struct ApInfo { char ssid[33]; int rssi; bool secure; };
int net_scan(ApInfo* out, int max_n);

// 换 WiFi:存进 NVS 并立即重连。ssid 为空表示只重连当前配置。
void net_set_credentials(const char* ssid, const char* password);

// ── 腾内部 RAM ────────────────────────────────────────────
// BLE 协议栈只能用内部 DRAM,而 WiFi/LWIP 的缓冲区也在那儿。
// 这不是 esp_wifi_stop() —— stop 只是断链,缓冲区还占着;
// 要真正还回堆里必须走到 esp_wifi_deinit() + 销毁 netif。
//
// ⚠️ 挂起期间调试接口(端口 80)也一起没了,推固件、截图都不通。
// 谁挂起谁负责恢复。
bool net_suspend();
void net_resume();
bool net_suspended();
}
