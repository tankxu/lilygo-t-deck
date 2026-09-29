# T-Deck OS

给 LilyGO T-Deck(ESP32-S3 / 320×240 / 全键盘 + 轨迹球 + 触屏 + 麦克风喇叭 /
16MB flash + 8MB PSRAM)写的一个带 launcher 的小系统,应用自己开发。

**单固件多应用**:开机进桌面,选 app,切换即时无重启 —— 不是 Meshtastic
那种单一用途固件,也不是"脚本小工具盒"。

ESP-IDF v5.5.2 + LVGL 9.1,C++。约 13,000 行。

## 应用

| | |
|---|---|
| **小智** | 语音助手。常驻后台:切走只收起 UI,连接和音频线程继续活着(轨迹球长按 = 全局语音键) |
| **学习卡片** | 汉字/词卡、笔顺动画、LaTeX 公式排版。挂成小智的 MCP 工具,对它说"这个字怎么写"就出笔顺 |
| **音乐** | 接自建的 xiaozhi-music-mcp,搜索/播放/封面墙 |
| **B站** | 配合局域网转码服务在 2 寸屏上看视频,320×180 @10fps 音画同步 |
| **Apple Music** | 车载遥控:读 iPhone 的 Apple Media Service 拿歌曲信息 + 走带控制。零 iPhone App、零开发者账号 |
| 设置 / 色板 / 自检 / 固件变体 | |

## 两个构建变体

Apple Music 那个要 BLE,而 `CONFIG_BT_ENABLED=y` 一开,蓝牙控制器就**无条件**
占掉 54KB 内部 DRAM —— 运行时关不掉。实测开着 BT 进音乐 app,内部堆最低点
掉到 **23 字节**,专辑封面全部加载失败。

所以分成两个固件,各装一个 OTA 槽:

```
daily   全部 app,蓝牙关          开机内部堆空闲 98KB
car     Apple Music,蓝牙开        开机内部堆空闲 79KB
        不编小智/B站/音乐
```

切换 = 改 otadata + 重启,**不写 flash**,实测 6 秒。应用页的「固件变体」卡片里按一下。

顺带一个反直觉的实测结论:**BLE 和 WiFi 在车载变体里是共存的**。
"装不下"只是日常固件的现象(被小智/esp-sr 占掉了),不是芯片的限制 ——
车载变体里 BLE 起来之后还剩 30KB,WiFi 照常响应 HTTP。

## 结构

```
lilygo-t-deck/
├── docs/
│   ├── hardware.md        引脚 / 外设 / 刷机模式
│   └── decisions.md       架构决策记录(ADR)
├── components/
│   ├── tdeck_bsp/         板级支持包 —— launcher 和各 app 共用
│   │   ├── display/         ST7789 + LVGL flush
│   │   ├── touch/           GT911
│   │   ├── keyboard/        I2C 0x55(独立的 ESP32-C3 键盘 MCU)
│   │   ├── trackball/       4 路方向 GPIO + 中键 → LVGL indev
│   │   ├── audio/           ES7210 输入 + I2S 直推功放
│   │   └── power/           GPIO10 外设总电源 + 电池 ADC
│   └── xiaozhi/           小智内核(只通过 xiaozhi_core.h 对外)
├── main/
│   ├── launcher/          LVGL 桌面
│   ├── learning/          学习卡片 + LaTeX 排版 + GIF 播放器
│   ├── net/               WiFi / NTP / 天气
│   ├── debug/             开发期 HTTP 调试接口
│   └── apps/              各应用
└── tools/                 构建 / 推固件 / 调试脚本
```

## 构建与刷写

⚠️ 按仓库根 `CLAUDE.md` 的约定,**ESP-IDF 构建走断网沙箱 `safe-build`**
(`idf.py` 会执行大量第三方 build 脚本)。`tools/build.sh` 已经包好了:

```bash
tools/build.sh            # daily 变体 → build/
tools/build.sh car        # 车载变体   → build.car/
tools/build.sh car clean  # 先 fullclean
```

两个变体各有独立的 build 目录和 sdkconfig —— 共用一份会来回触发全量重编,
而且 BT 的几十个子符号一旦被 confgen 固化进 sdkconfig,关掉父开关也不会跟着撤销。

推固件走 WiFi,不用拔插 USB:

```bash
tools/ota.sh              # 推 daily
tools/ota.sh car          # 推 car
```

写的是**当前没在跑的那个槽**,推挂了最坏就是白推一次(新固件起来后要自己
声明可用,否则 bootloader 自动回滚)。脚本会拦住"跑着 X 又推 X"——
那会把另一个变体覆盖掉。

第一次刷需要 USB:`idf.py flash` —— 它同时写 `assets` 和 `model` 分区,
而 OTA **只写 app 分区**。

## 配置

复制 `main/secrets.h.example` 成 `main/secrets.h` 填自己的值(WiFi、
自建服务地址等)。这个文件在 `.gitignore` 里,不会进版本库。

## 开发期调试接口

联网后设备在 80 端口开一组 HTTP 接口,用来在电脑上闭环调试 UI:

```
GET /shot                    截图(裸 RGB565 320×240×2)
GET /tap?x=&y=  /swipe=      注入点击/滑动(走真实 indev,和手指同一条路径)
GET /key?c=  /ball?d=        注入键盘/轨迹球
GET /info  /mem              堆、分区、变体、内存探针
GET /learn?text=|stroke=|formula=   学习卡片,不用对着小智说话就能验
POST /ota                    推固件
```

⚠️ **没有任何鉴权,只在同一局域网可达就能用。发布前必须关掉。**

## 关键决策

完整记录见 [docs/decisions.md](docs/decisions.md)。

| | |
|---|---|
| **不套 Lua 解释器** | ADR-001。套 Lua 就必须走双固件,切 app 要重启,掌上机上体验不成立 |
| **BSP 独立成 component** | ADR-002。launcher 和每个 app 共用,也是唯一和硬件强绑定的一层 |
| **小智作为 app 插进 launcher** | ADR-003。C++ `App` 接口 + 注册表,切换是 `lv_scr_load_anim` 不是重启 |
| **小智常驻后台 + 悬浮窗** | ADR-004。UI 建在 `lv_layer_top()`,浮在任何 app 之上 |
| **键位语义在 OS 层定义** | ADR-006。`0` 进小智、`y`/`n` 确认取消、`i`/`o` 音量、`q` 返回、`s` 快捷键表 |
| **双槽 OTA + 两个变体** | ADR-007。**推翻了 ADR-005 的"不做 OTA"** —— 改一行 UI 就要拔插 USB 刷 40 秒,那条链路才是真正的瓶颈 |

## 为什么不 fork 现成的 T-Deck OS

调研过四个:

| 项目 | ★ | 许可 | 评价 |
|---|---|---|---|
| `zobithecat/t-deck-os` | 2 | MIT | 硬件覆盖对得上,但太新太小。**驱动写法值得参考** |
| `hermes-gadget/SigurdOS-tdeck` | 26 | GPL-3.0 | 成熟度最高。**UI 布局值得参考**;GPL-3 有传染性,不作基座 |
| `wan0net/thistle-os` | 12 | BSD-3 | Rust 写的,而且是给 T-Deck **Pro**(墨水屏)的,硬件对不上 |
| `luisriverag/tdeck-launcher` | 0 | MIT | 是"刷别的固件的启动器",不是 OS |

结论:**不 fork,自己写**,借鉴前两个的驱动写法和 UI 布局。
主要原因是第一个应用是小智 —— 基座得选在小智那边(见 ADR-003),
而这四个项目谁都没有音频栈。

## 外部依赖

设备本身只要 WiFi,但几个应用接的是自建/第三方服务:

| 应用 | 服务 |
|---|---|
| 音乐 | [xiaozhi-music-mcp](https://github.com/tankxu/xiaozhi-music-mcp)(自建) |
| B站 | stackchan-bili 的转码服务(自建,同局域网) |
| 学习卡片 | 预渲染的汉字卡/笔顺 GIF,R2 + CDN 直出 |
| 小智 | 小智官方服务 |
| 天气 | Open-Meteo(免费,不要 key) |
| Apple Music | 不需要服务 —— 直接读 iPhone 的 AMS |
