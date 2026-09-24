# components/xiaozhi —— 小智核心

按 ADR-003 的路线 B:**只搬子系统,不继承上游的构建体系**。

上游 `main/CMakeLists.txt` 有 1206 行、252 个条件分支,外加字体生成、语音资源
打包、200 块板子的选择逻辑。整个继承过来意味着每次跟上游同步都要重做一遍,
而我们真正需要的只有四块:

| 搬进来的 | 用途 |
|---|---|
| `audio/` | Opus 编解码、AFE(回声消除/降噪)、音频服务 |
| `protocols/` | WebSocket / MQTT,和小智服务端说话 |
| `application.cc` | 状态机(idle / listening / speaking) |
| `mcp_server.cc` | 让服务端能调设备上的工具 |

板级和显示由 launcher 的 `tdeck_bsp` 提供,上游那套 `Board`/`Display` 抽象
只保留基类。

## 版本必须钉死的两个依赖

**`78/esp-ml307`** —— 整份拷进 `components/esp-ml307`,因为小智要的 `~0.2.1`
**已从组件仓库下架**。现在拉 `"*"` 得到 3.7.5,那一版删掉了
`Mqtt::GetLastError()` / `WebSocket::GetLastError()`,protocols 层直接编不过。

**`espressif/esp-sr ~2.3.0`** —— 别被小智 `dependencies.lock` 里的 1.7.0 误导,
那是传递依赖。`afe_audio_processor.cc` 用的是 2.x 的 API
(`afe_config_init` / `AFE_TYPE_VC` / `enable_vad`),1.7.0 上一片编译错误。

## 另外两处必须的工程配置

- `CONFIG_COMPILER_CXX_RTTI=y` —— `audio_service.cc` 用 `dynamic_cast` 区分 codec 子类
- `BOARD_NAME` / `BOARD_TYPE` 宏由本组件的 CMakeLists 直接给死(上游是按所选板子
  注入的)。注意必须放在 `idf_component_register` **之后**用
  `target_compile_definitions` —— 组件 CMakeLists 会被 IDF 先跑一遍"只收集依赖"
  的模式,那个模式下 `add_compile_definitions` 根本不存在。

## 当前进度

组件**能编译进固件**,但还没有接线:`Application::Initialize()` 需要一个
`Board` 实现(`create_board()`),那是下一步 —— 用 `tdeck_bsp` 做一层薄适配,
把 I2C 总线、显示句柄、背光转发过去,而不是让它自己再初始化一遍硬件
(会和 launcher 抢 LEDC TIMER_0、I2C port、SPI host)。
