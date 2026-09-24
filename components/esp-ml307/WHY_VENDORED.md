# 为什么把 esp-ml307 本地化

小智 2.2.6 依赖 `78/esp-ml307 ~0.2.1`,提供 Http / WebSocket / Mqtt / Udp 的统一抽象,
`components/xiaozhi` 的 protocols 层直接用它的接口。

这个版本**已经从组件仓库下架**:现在拉 `"*"` 会得到 3.7.5,而那一版把
`Mqtt::GetLastError()` / `WebSocket::GetLastError()` 删了,小智的 protocols 层
直接编不过。小智自己能编是因为它的 dependencies.lock 里有缓存。

所以整份拷进来固定住。副本来自 xiaozhi-esp32 v2.2.6 的 managed_components。
