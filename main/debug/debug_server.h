#pragma once
namespace tdeck {
// 开发期调试接口。联网后调用一次即可,端口 80。
void debug_server_start();

// 收掉服务、释放 socket。net_suspend() 要用 —— WiFi 拆掉之前
// httpd 必须先停,否则它还抓着 lwip 的资源。
// start() 是幂等的,恢复后重复调没关系。
void debug_server_stop();
}
