// main.cc — 启动:BSP → LVGL → 进入第一个注册的 app
//
// launcher 还没写(M2),所以这里暂时直接进注册表里的第一个 app。
// 等 launcher 到位,这段换成「加载 launcher screen」,其余不用动。

#include "app.h"
#if TDECK_LEARNING
#include "learning/learning.h"
#endif
#include "net/net.h"
#include "sys/volume.h"
#include "ui/fonts.h"
#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char* TAG = "main";

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(tdeck_bsp_init());

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // 默认 7168 不够:launcher 会用 lv_snapshot_take 把整页渲染成位图
    // (滑动时贴图代替重画,见 launcher.cc 的 bake_page),
    // 整页渲染要走完 LVGL 的绘制递归,栈很深。
    // ⚠️ 别从 app_main 里调 snapshot —— 主任务栈更小,实测直接爆栈
    // (vApplicationStackOverflowHook),所以烘图也挪到 LVGL 任务里做。
    port_cfg.task_stack = 16384;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = tdeck_get_panel_io(),
        .panel_handle  = tdeck_get_panel(),
        // 滑动时卡片竖边上的"锯齿",根子在【一帧被拆成几条横带分别送出去】:
        // 每条带渲染+发送的时刻不同,而画面在移动,于是相邻带之间错开几像素,
        // 竖直边就被切成一段段台阶。带越多,台阶越多。
        //
        // 整屏一块是最彻底的解法,但 320x240x2 = 150KB,内部 RAM 放不下;
        // 放 PSRAM 试过 —— buff_spiram 无论配不配 DMA 标志,SPI 传输都完不成,
        // LVGL 卡死在等刷屏上(现象是开机走到联网就不动、且没有任何错误日志)。
        //
        // 半屏单缓冲也试过:接缝确实从 5 道减到 1 道,但失去了渲染/传输重叠,
        // 刷屏从 3ms 变成阻塞的 16ms,帧率反而从 35 掉到 26,
        // 而且内部 RAM 只剩 24K(小智还要用)。不划算。
        //
        // 所以维持 40 行双缓冲:实测 25ms/帧、35 FPS、内部 RAM 还剩 ~71K,
        // 是量下来最好的一组。锯齿是这块内存预算下的固有代价。
        // ⚠️ 40 行是按【渲染帧率】选的(25ms/帧、35 FPS),但它吃 51,200 字节
        // 内部 DMA 内存(320*40*2 双缓冲),而小智起来之后内部堆就见底了:
        // 实测只剩 11KB、最大连续块 7680 —— 激活任务(要 8192 连续)建不起来,
        // TLS 握手时 esp-aes 也分配不到 DMA 内存,小智根本连不上。
        // 降到 24 行省出 20,480 字节。代价是滑动时横带接缝更多一点。
        .buffer_size   = TDECK_LCD_H_RES * 24,
        .double_buffer = true,
        .hres          = TDECK_LCD_H_RES,
        .vres          = TDECK_LCD_V_RES,
        .monochrome    = false,
        // ⚠️ 这三个值必须跟 tdeck_display_init() 里设的一致。
        // lvgl_port_add_disp() 结尾会调 lvgl_port_disp_rotation_update(),
        // 无条件用这里的值去 esp_lcd_panel_swap_xy/mirror 覆盖面板设置 ——
        // 留 false 的话面板会被打回 240x320 竖屏,而 LVGL 仍按 320 宽喂数据,
        // 每行错位累积,画面就成了竖条纹。
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            // 缓冲必须放内部 RAM 且具备 DMA 能力。放 PSRAM 会花屏:
            // esp_lcd 的 SPI 通道要对缓冲做 DMA,而 ESP32-S3 的 SPI DMA 读 PSRAM
            // 有对齐和 cache 同步的约束。双缓冲共 50KB,内部 RAM 放得下。
            // ⚠️ 缓冲必须在内部 RAM 且可 DMA。放 PSRAM 试过两种标志组合,
            // SPI 传输都完不成,LVGL 就卡死在等 on_color_trans_done 上,
            // 而且一条错误日志都没有 —— 表现是开机走到联网那一步就不动了
            // (联网回调要拿 LVGL 锁,被一起堵住)。
            .buff_dma    = true,
            .buff_spiram = false,
            // SPI 屏收的是大端 RGB565,LVGL 渲染出来是小端,必须翻字节序。
            .swap_bytes  = true,
        },
    };
    lv_display_t* disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);

    // 触摸接给 LVGL —— 有了它卡片才能点、屏幕才能滑
    esp_lcd_touch_handle_t tp = tdeck_touch_init();
    if (tp) {
        lvgl_port_touch_cfg_t tcfg = { .disp = disp, .handle = tp };
        lvgl_port_add_touch(&tcfg);
        ESP_LOGI(TAG, "触摸已接入 LVGL");
    } else {
        ESP_LOGW(TAG, "触摸不可用,只能用键盘和轨迹球");
    }

    auto& apps = tdeck::AppRegistry::instance().apps();
    ESP_LOGI(TAG, "已注册 %d 个 app", (int)apps.size());
    for (auto* a : apps) ESP_LOGI(TAG, "    - %s", a->name());
    if (apps.empty()) {
        ESP_LOGE(TAG, "注册表是空的 —— 检查 main/CMakeLists.txt 有没有 WHOLE_ARCHIVE");
    }

    tdeck::volume::init();   // 读回上次的音量,任何人放声音之前

    lvgl_port_lock(0);
    tdeck::fonts_init();   // launcher 建 UI 之前要先有字体
    tdeck::launcher_begin();
#if TDECK_LEARNING
    // 学习卡片盖在 lv_layer_top 上,和 launcher 无关,但它建的 lv_timer
    // 必须在持锁时建 —— LVGL 的对象和定时器都不是线程安全的
    tdeck::learning::begin();
#endif
    lvgl_port_unlock();

#if TDECK_LEARNING
    // MCP 工具要在小智 Application 启动【之前】注册(见 learning.h)
    tdeck::learning::register_mcp();
#endif

    // 白底把背光漏光盖住了,不必为此压低亮度
    tdeck_backlight_set(70);

    // 联网放最后:桌面先出来,时间天气等联上了自己填进去,不让开机卡在连 WiFi 上
    tdeck::net_start();
}
