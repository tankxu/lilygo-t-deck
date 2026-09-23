# 架构决策记录

## ADR-001:不套 ESP-Claw 的 Lua 解释器

**日期**:2026-09-23
**状态**:已决定
**背景**:sticks3(`esp-claw-sticks3`)上,应用全部是 FATFS 里的 Lua 脚本,
`startup.lua` 是个 135x240 的双页 launcher,小智作为第二固件放在 ota_1,
按键切换启动分区。T-Deck 要不要复制这套?

### 结论:不复制。T-Deck 走单固件 + C/C++ + LVGL。

### 理由

**1. 双固件的重启代价在 T-Deck 上不可接受**

ESP-Claw 和小智是两个独立的 ESP-IDF 工程,无法合并进一个 app 分区。
套 Lua 就必然走 sticks3 的老路:launcher 在 ota_0,小智在 ota_1,
切换要翻转启动分区 + 重启。

135x240 的 StickC 上这是可以忍的 —— 它本来就是"一次干一件事"的小玩意。
但 T-Deck 有键盘有触屏,定位是掌上机,点一个 app 要黑屏重启三秒,
"OS"的连续感当场就碎了。单固件里切换 LVGL screen 是零延迟的。

**2. ESP-Claw 没有 T-Deck 板级支持,省不下工作量**

`esp-claw-sticks3` 里只有 `m5stack/m5stack_sticks3` 和 `espressif/esp_Ditto` 两块板。
上 T-Deck 要写 `board_info.yaml` / `board_devices.yaml` / `board_peripherals.yaml` /
`setup_device.c` / `power_manager.c`,再加触屏、键盘、轨迹球、ES7210 的 Lua 绑定。
这个量不比直接写 C++ 驱动少,而且写完还多背一层解释器。

**3. Lua 的核心卖点对本项目不成立**

ESP-Claw 的 Lua 是为"聊天即创造"设计的 —— 让不会编程的人用对话生成设备行为,
免编译、动态加载。本项目的目标是"自己开发多个应用",开发者自己写代码,
重新编译刷机不是瓶颈;反倒是 Lua 绑定会卡住能力上限(小智要的 Opus 编解码、
WebSocket、音频线程,都得穿透到 C 层)。

**4. 资源不紧张,不需要靠脚本省空间**

16MB flash / 8MB PSRAM(实测)。sticks3 当年挤在 8MB flash 里要做双固件融合、
让 fonts 分区给 storage 腾 0xC000,那种紧绷在这里不存在。
单固件塞下 launcher + 若干 app 很宽裕。

### 保留的后路

如果后期确实想要"脚本化小工具"(临时自动化、配置化的小程序),
可以把一个精简 Lua(lua 5.4 + 少量绑定)作为**其中一个 app** 嵌进来,
而不是让整个 OS 建在解释器之上。两头的好处都能拿到,代价是可控的。

---

## ADR-002:BSP 做成独立 component

**日期**:2026-09-23
**状态**:已决定

显示、触摸、键盘、轨迹球、音频、电源这些驱动,不写进任何一个 app,
而是收进独立的 `components/tdeck_bsp`。

理由:launcher 和每一个 app(小智在内)都要用这套驱动,不该由谁私有。
收成独立 component 后,app 只依赖 BSP 的接口,换实现不影响上层;
同时这层也是唯一和 T-Deck 硬件强绑定的代码,将来换板子只动它。

---

## ADR-003:小智作为 app 插进 launcher(路线 B)

**日期**:2026-09-23
**状态**:已决定

### 决定

自研 launcher 做基座,定义 C++ `App` 接口 + 静态注册表,
小智实现这个接口作为第一个 app 插进来。**单固件,不切分区。**

灵感来自 Stack-chan 的 mod 架构(基座固件 + 可插拔改装),
但插口形态不同:Stack-chan 插的是同一个机器人应用的行为变体,脚本足够;
这里插的是独立的完整应用(小智要音频线程、Opus、WebSocket),
所以插口是 C++ 抽象基类,不是脚本运行时。

```cpp
class App {
    virtual const char* name() const = 0;
    virtual const void* icon() const = 0;
    virtual void on_enter(lv_obj_t* root) = 0;  // launcher 递一块 screen 过来
    virtual void on_exit() = 0;                 // 必须还干净
    virtual void on_key(int keycode) = 0;       // 键盘 / 轨迹球 / 触屏
};
```

### 为什么不用切分区

sticks3 当年切分区不是 Lua 的锅,是**两个工程的锅**:ESP-Claw 和小智
各自是完整的 ESP-IDF application,各有各的 `app_main()`,一个 ELF 只能有一个入口,
只能分别烧进 ota_0 / ota_1 靠 `esp_ota_set_boot_partition()` + 重启来切。

插口架构下所有 app 编译进同一个 binary,只有一个 `app_main()`。
launcher 和小智是同一程序里的两个对象,切换就是函数调用 +
`lv_scr_load_anim()`,不重启、不动 flash,量级几十到几百毫秒。

### 代价

1. **RAM 共享,flash 不共享**。代码常驻 flash(16MB 够),运行时内存共享。
   纪律:`on_enter` 申请的,`on_exit` 必须还干净。8MB PSRAM 让这件事宽松,
   但规矩不能破。

2. **小智要改造成可启停** —— 本路线的主要工作量。它的 `Application` 是单例,
   设计上假定独占设备生命周期(开机起音频线程、连服务器、跑到断电)。
   分两步做:先让它**常驻后台**(退出只隐藏 UI、保留连接)拿到能跑的第一版,
   再补真正的 teardown。

3. **分区表重算**(16MB):

   | 分区 | 大小 | 说明 |
   |---|---|---|
   | `factory` / `ota_0` | 6MB | 全部 app |
   | `ota_1` | 6MB | 要 OTA 才留;有线刷的话可砍掉 |
   | `storage` (FATFS) | 3MB+ | 字体、图标、配置、录音缓存 |

   参考量级:小智固件本体约 3–4MB,T-Deck 版 Meshtastic TFT 实测 3.6MB。
   不做 OTA 的话 `factory` 能给到 10MB+。

### 脚本化的后路不变

后期想要脚本小工具,把精简 Lua 当**一个 app** 插进来(`apps/lua/`),
跟 ADR-001 留的后路正好接上。

---

## ADR-004:小智常驻后台 + 悬浮窗唤醒

**日期**:2026-09-23
**状态**:已决定

### 决定

小智不走普通 app 的"退出即销毁",而是**常驻后台**:
连接、音频线程、唤醒检测一直活着,只有 UI 在收起时隐藏。
按语音键随时唤醒,以**悬浮窗**形态浮在当前 app 之上,不打断底下正在做的事。

### 实现机制

LVGL 的分层能力正好对上:

| 层 | 用途 |
|---|---|
| `lv_scr_act()` | 当前 app 的 screen,`lv_scr_load_anim()` 切换 |
| **`lv_layer_top()`** | **小智悬浮窗** —— 浮在任何 screen 之上,且不随 screen 切换销毁 |
| `lv_layer_sys()` | 状态栏 / 全局提示 |

所以小智的 UI 建在 `lv_layer_top()` 上。底下 app 怎么切,悬浮窗都在;
收起就是把这一层 `lv_obj_add_flag(..., LV_OBJ_FLAG_HIDDEN)`,零销毁零重建。

### 语音键选谁

**轨迹球中键长按**(GPIO0)。理由:它是板上唯一独立于键盘矩阵的物理按键,
做全局热键不会和任何 app 的文字输入冲突。短按仍然留给当前 app 当确认键。

> GPIO0 同时是 BOOT —— 启动之后它就是一个普通带上拉的 GPIO,运行期复用没问题。

### 对 App 接口的影响

`App` 接口要能表达"我要后台常驻":

```cpp
class App {
    // ...
    virtual bool wants_background() const { return false; }  // 小智返回 true
    virtual void on_background() {}   // UI 收起,但线程/连接继续
    virtual void on_foreground() {}   // 重新浮出
};
```

launcher 切走时:`wants_background()` 为真的调 `on_background()`,
否则照常 `on_exit()` 彻底释放。

### 代价

小智常驻意味着它的 RAM 不会被回收 —— 音频 buffer、Opus 编解码器、
WebSocket 收发缓冲一直占着。8MB PSRAM 扛得住,但这是唯一一个享受此待遇的 app,
其它 app 一律 `on_exit()` 还干净(ADR-003)。

---

## ADR-005:不做 OTA,分区按空间最大化切

**日期**:2026-09-23
**状态**:已决定

设备有线刷方便(见 `hardware.md` 的下载模式流程),不值得为 OTA 留一个
对等大小的备份槽。16MB 全部用于单槽 app + 大容量存储。

```
# Name,     Type, SubType,  Offset,    Size        说明
nvs,        data, nvs,      0x9000,    0x6000      # 配置 / WiFi 凭据
phy_init,   data, phy,      0xf000,    0x1000
factory,    app,  factory,  0x10000,   0x800000    # 8MB —— launcher + 全部 app
storage,    data, fat,      0x810000,  0x7E0000    # 8064KB —— 字体/图标/录音/缓存
coredump,   data, coredump, 0xFF0000,  0x10000     # 64KB —— bring-up 期间排错用
```

校验:`0x810000 + 0x7E0000 = 0xFF0000`,`0xFF0000 + 0x10000 = 0x1000000` = 16MB,刚好占满。

**为什么 app 槽给 8MB 而不是更多**:小智固件本体约 3–4MB,
T-Deck 版 Meshtastic TFT 实测 3.6MB。8MB 足够塞下 launcher + 小智 + 十几个应用,
剩下的给 storage 更有价值 —— 中文字体、图标资源、录音缓存都吃空间。
真不够了再从 storage 划过来即可(有线刷,改分区表零成本)。

**coredump 值得留**:驱动 bring-up 阶段 panic 是家常便饭,
64KB 换 `idf.py coredump-info` 能直接看到崩溃现场,便宜得离谱。
