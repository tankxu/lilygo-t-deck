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
