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
}
