#pragma once
namespace tdeck {
// 启动联网:WiFi → SNTP 对时 → 定期取天气。全程异步,不阻塞 app_main。
void net_start();
}
