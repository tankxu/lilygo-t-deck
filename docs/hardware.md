# LilyGO T-Deck 硬件参考

来源:`Xinyuan-LilyGO/T-Deck` 官方 `examples/UnitTest/utilities.h`,
外加 2026-09-23 本机 esptool 实测。

## 主控实测值

```
Chip:   ESP32-S3 (QFN56) rev v0.2
RAM:    Embedded PSRAM 8MB (AP_3v3)
Flash:  16MB, 厂商 c8 / 型号 4018, quad(4 数据线), eFuse 电压 3.3V
USB:    USB-Serial/JTAG(原生,无 USB-TTL 芯片)
MAC:    3c:84:27:cc:63:c8
```

## 引脚表

### ⚠️ 外设总电源

| 名称 | GPIO | 说明 |
|---|---|---|
| `BOARD_POWERON` | **10** | **必须最先拉高**。不拉高则屏幕、I2C、音频全部不工作 —— T-Deck 移植第一大坑 |

### 显示 ST7789 (320x240)

| 信号 | GPIO |
|---|---|
| `TFT_CS` | 12 |
| `TFT_DC` | 11 |
| `TFT_BACKLIGHT` | 42 |

### SPI 共享总线

| 信号 | GPIO |
|---|---|
| `MOSI` | 41 |
| `MISO` | 38 |
| `SCK` | 40 |

挂在这条总线上的三个从设备,靠各自 CS 区分:

| 设备 | CS |
|---|---|
| ST7789 屏幕 | 12 |
| SD 卡 | 39 |
| SX1262 LoRa | 9 |

### I2C 共享总线

| 信号 | GPIO |
|---|---|
| `SDA` | 18 |
| `SCL` | 8 |

挂载设备:键盘 MCU(0x55)、触摸 GT911、音频 ADC ES7210。

### 音频

输出 —— I2S 直推 class-D 功放,**板上没有输出 codec**:

| 信号 | GPIO |
|---|---|
| `I2S_WS` | 5 |
| `I2S_BCK` | 7 |
| `I2S_DOUT` | 6 |

输入 —— **ES7210** 四通道音频 ADC,I2C 配置 + 独立 I2S:

| 信号 | GPIO |
|---|---|
| `ES7210_MCLK` | 48 |
| `ES7210_LRCK` | 21 |
| `ES7210_SCK` | 47 |
| `ES7210_DIN` | 14 |

> ES7210 是小智原生支持的芯片(ESP-BOX / Korvo / M5 CoreS3 都在用),
> 这是选它做第一个应用的主要原因。

### 输入设备

| 设备 | GPIO | 说明 |
|---|---|---|
| 轨迹球 上/下/左/右 | 3 / 2 / 15 / 1 | 官方命名 `TBOX_G01..G04`,四路正交方向脉冲 |
| 轨迹球 中键 | **0** | 与 BOOT 共用 —— 这就是刷机时"按住轨迹球"的原理 |
| 触摸中断 | 16 | GT911 |
| 键盘中断 | 46 | |

### 其它

| 名称 | GPIO | 说明 |
|---|---|---|
| 电池 ADC | 4 | |
| LoRa BUSY | 13 | SX1262 |
| LoRa RST | 17 | |
| LoRa DIO1 | 45 | |
| GPS TX / RX | 43 / 44 | 仅 T-Deck Plus |

## 键盘(独立 MCU)

键盘是一颗**独立的 ESP32-C3**,跑自己的固件,通过 I2C `0x55` 把键值送给主控。

- 刷键盘固件要走 RST 旁边的 6-pin 排针(3V3/GND/RST/BOOT/RX/TX)+ 外接 USB-TTL,
  跟刷主控完全是两条路。
- 主控侧只需读 I2C,不用管扫描矩阵。

## 刷机模式

没有 BOOT 按键,BOOT 就是轨迹球中键(GPIO0);ESP32-S3 是原生 USB,
没有自动复位电路,esptool 的 DTR/RTS 套路无效。

手动进下载模式:

1. 侧面电源开关拨 **OFF**
2. USB-C 接电脑(必须是数据线)
3. **按住轨迹球下压**不放
4. 电源开关拨 **ON**
5. 继续按住 2~3 秒再松手

判据:屏幕全黑且背光不亮。此时枚举为 `USB JTAG/serial debug unit`(VID `0x303A` / PID `0x1001`)。

> 注意:插在 USB-C 扩展坞上容易因供电不足反复重新枚举而拿不到串口,
> 刷机时直插电脑。

### 为什么不像 M5Stack 那样插上就能刷

M5Stack 板上有 CH9102F / CP2104 这类 **USB 转串口桥接芯片**,它的 DTR/RTS
经一个两颗三极管的自动复位电路接到 EN 和 GPIO0。esptool 一翻 DTR/RTS,
硬件就把芯片拽进下载模式,永远不用碰按钮。

**T-Deck 上这颗桥接芯片和这套电路都没有** —— USB-C 直连 ESP32-S3 的原生 USB。
所以那套自动复位在物理上就不存在。

但 ESP32-S3 的 USB-Serial/JTAG 外设**自身**支持 esptool 复位,不需要外部电路。
前提是当前运行的固件把 USB 留给这个外设:

| 固件 | USB 归谁 | esptool 自动复位 |
|---|---|---|
| Meshtastic | TinyUSB CDC 接管 | ❌ 必须手按轨迹球 |
| 本项目 | 原生 USB-Serial/JTAG(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) | ✅ |

所以 sdkconfig 里那条 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 不只是为了看日志,
它同时决定了往后还要不要手动进下载模式。**别改成 TinyUSB CDC**,
除非你愿意每次刷机都按轨迹球。

### ⚠️ 不要手工 open /dev/cu.* 读串口

macOS 打开 tty 时会默认拉起 DTR,而在 ESP32-S3 的 USB-Serial/JTAG 上
**DTR 直通 GPIO0**。也就是说,自己写脚本 `open("/dev/cu.usbmodem*")` 读日志,
等于把 BOOT 键一直摁着 —— 之后每次复位都掉回下载模式,app 永远起不来,
而且现象是"串口在、但一个字节都读不到",极具误导性。

一律用 `idf.py monitor`,它知道怎么正确处理 DTR/RTS。

## 显示:三个踩过的坑

### 1. esp_lvgl_port 会覆盖面板的方向设置

`lvgl_port_add_disp()` 结尾调 `lvgl_port_disp_rotation_update()`,
用 `lvgl_port_display_cfg_t.rotation` 里的值**无条件**执行
`esp_lcd_panel_swap_xy()` / `esp_lcd_panel_mirror()`。

所以在 `tdeck_display_init()` 里设的方向会被打回去。两处必须写一样的值:

```
swap_xy = true, mirror_x = true, mirror_y = false     // T-Deck 横屏,已在真机验证
```

不一致的症状:面板退回 240x320 竖屏而 LVGL 仍按 320 宽送数据,
每行错位累积,画面变成竖条纹 —— 看起来很像"接线错了",实际是软件。

### 2. ST7789 的 gamma 要用 T-Deck 专用值

IDF 通用 ST7789 驱动只发 SLPOUT / MADCTL / COLMOD / RAMCTRL,
电压和 gamma 用出厂默认。症状:饱和色正常,近黑区域压不到真黑、泛红带渐变。

补 LilyGO 官方 `Setup210_LilyGo_T_Deck.h` 选中的 `INIT_SEQUENCE_2`,
共 11 条寄存器,关键是 `VCOMS(0xBB) = 0x1A` —— VCOM 电压直接决定黑电平。
该序列末尾有 `INVON`,说明 `invert_color(true)` 是对的。

### 3. draw_bitmap 是异步的,缓冲不能立刻 free

`esp_lcd_panel_draw_bitmap()` 对 SPI 面板把传输排进队列就返回,DMA 还在后台读内存。
立刻 `free()` 或改写缓冲,DMA 会把新数据当成旧传输发出去。

症状很有迷惑性:屏幕某处出现一条几像素高的异色横带,而且**左右两段错开**
(改写只覆盖了缓冲的一部分,边界落在某一行中间)。

要用 `on_color_trans_done` 回调等传输完成。`esp_lvgl_port` 内部已经这么做了,
所以只有自己直接调 `draw_bitmap` 时才会踩。

## 显示:一个不是 bug 的现象

**四角红光 = 背光漏光**,硬件特性,不是故障,软件治不了。

已验证:全黑画面下扫背光亮度,红光成比例变化、0% 时完全消失 ——
说明光源是背光而非面板异常。另外换 Meshtastic(完全不同的显示驱动栈)
同样能看到,进一步排除软件因素。

为什么只在深色画面上看得见:LCD 显示黑色时背光并没有关,
只是液晶挡光(挡不干净,且边缘的光绕过液晶层直接漏出)。
漏光的量恒定,亮画面时被完全淹没,黑画面时成了屏上唯一的光。

缓解手段:背光别开太高(40~50% 观感明显改善且省电),
UI 用深灰底(如 `#0d1117`)而不是纯黑。
