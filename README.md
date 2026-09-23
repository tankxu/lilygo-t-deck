# T-Deck OS

给 LilyGO T-Deck(ESP32-S3 / 320x240 / 键盘 + 轨迹球 + 触屏 + 麦克风喇叭)
写一个带 launcher 的小系统,应用自己开发。

设备已于 2026-09-23 确认可正常刷写,当前跑的是 Meshtastic
(`firmware-t-deck-tft-2.7.26`)。本项目会覆盖掉它。

## 定位

不是 Meshtastic 那种单一用途固件,也不是 sticks3 那种"脚本小工具盒",
而是一个**单固件多应用**的掌上机系统:开机进 launcher,选 app,
切换即时无重启。

第一个应用:**小智**(语音助手)。

## 结构

```
lilygo-t-deck/
├── docs/
│   ├── hardware.md      引脚 / 外设 / 刷机模式(已完成)
│   └── decisions.md     架构决策记录(ADR)
├── components/
│   └── tdeck_bsp/       板级支持包 —— 独立 component,launcher 和各 app 共用
│       ├── display/       ST7789 + LVGL flush
│       ├── touch/         GT911
│       ├── keyboard/      I2C 0x55,独立 ESP32-C3 键盘 MCU
│       ├── trackball/     4 路方向 GPIO + 中键,映射成 LVGL indev
│       ├── audio/         ES7210 输入 + I2S 直推功放输出
│       └── power/         GPIO10 外设总电源 + 电池 ADC
├── main/
│   ├── launcher/        LVGL 桌面
│   └── apps/
│       └── xiaozhi/     第一个应用
└── tools/
```

## 关键决策

| | |
|---|---|
| **不套 ESP-Claw 的 Lua 解释器** | 见 [ADR-001](docs/decisions.md)。核心理由:套 Lua 就必须走双固件,切 app 要重启,掌上机上体验不成立 |
| **BSP 独立成 component** | 见 ADR-002。launcher 和每个 app 共用,也是唯一和硬件强绑定的一层 |
| **小智作为 app 插进 launcher** | 见 ADR-003。C++ `App` 接口 + 注册表,单固件不切分区,切换是 `lv_scr_load_anim` 不是重启 |
| **小智常驻后台 + 悬浮窗** | 见 ADR-004。UI 建在 `lv_layer_top()`,浮在任何 app 之上;轨迹球中键长按 = 全局语音键 |
| **不做 OTA,16MB 占满** | 见 ADR-005。`factory` 8MB + `storage` 8MB,有线刷不值得为 OTA 留对等备份槽 |

## 为什么不直接 fork 现成的 T-Deck OS

调研过四个:

| 项目 | ★ | 许可 | 评价 |
|---|---|---|---|
| `zobithecat/t-deck-os` | 2 | MIT | 硬件覆盖对得上(触屏/轨迹球/键盘),但太新太小,质量未经检验。**驱动写法值得参考** |
| `hermes-gadget/SigurdOS-tdeck` | 26 | GPL-3.0 | 成熟度最高,4x3 图标网格 launcher。**UI 布局值得参考**;GPL-3 有传染性,不作基座 |
| `wan0net/thistle-os` | 12 | BSD-3 | Rust 写的,而且是给 T-Deck **Pro**(墨水屏)的,硬件对不上 |
| `luisriverag/tdeck-launcher` | 0 | MIT | 是"刷别的固件的启动器",不是 OS,跑题 |

结论:**不 fork,自己写**,借鉴前两个的驱动写法和 UI 布局。
主要原因是第一个应用是小智 —— 基座得选在小智那边(见 ADR-003),
而这四个项目谁都没有音频栈。

## 路线图

- [ ] **M0 环境** —— 装 ESP-IDF v5.4.2(本机 9-22 重装后还没配)
- [ ] **M1 BSP + 自检应用** —— GPIO10 上电 → 屏幕 → 触摸 → 键盘 → 轨迹球 → 音频闭环
- [ ] **M2 Launcher 骨架** —— `App` 接口 + 注册表 + LVGL 图标网格 + screen 切换
- [ ] **M3 小智** —— T-Deck board + `tdeck_audio_codec`,常驻后台 + 悬浮窗
- [ ] **M4 更多应用**

M1 和自检应用是绑在一起做的 —— [自检](docs/apps/selftest.md)就是 BSP 的验收标准,
每写完一个驱动自检里多亮一站,七站全绿才算驱动层能交付。
第 5 站的"回放刚录的 3 秒"是麦克风→功放的端到端闭环,
一次跑通就等于小智需要的整条音频链路通了。

M2 先于 M3,是因为插口定下来小智才知道往哪插。

## 构建

⚠️ 按本仓库根 `CLAUDE.md` 的约定,**ESP-IDF 构建走 `safe-build`**
(`idf.py` 会执行大量第三方 build 脚本):

```bash
safe-build -- idf.py -DIDF_TARGET=esp32s3 build
```

刷写走断网之外的正常流程,设备串口见 `docs/hardware.md`。
