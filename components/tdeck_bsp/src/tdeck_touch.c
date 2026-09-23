// tdeck_touch.c — GT911 电容触摸
//
// 两个坑:
//
// 1. GT911 的 I2C 地址由上电瞬间 INT 引脚的电平决定:低 → 0x5D,高 → 0x14。
//    T-Deck 没有把这根线接到可控的地方,所以上电后到底是哪个地址不确定,
//    必须两个都探一遍。只写死一个的话在某些板子上就是"触摸完全没反应"。
//
// 2. 方向参数【不等于】显示的方向参数,不能想当然照抄:
//
//        显示  swap_xy=true, mirror_x=true,  mirror_y=false
//        触摸  swap_xy=true, mirror_x=false, mirror_y=true     ← 两个 mirror 是反的
//
//    触摸面板贴合的物理朝向和 LCD 的扫描方向本来就是两回事。
//    取值以 LilyGO 官方 UnitTest.ino 为准:
//        touch.setMaxCoordinates(320, 240);
//        touch.setSwapXY(true);
//        touch.setMirrorXY(false, true);
//
//    我最初按"和显示保持一致"去配,两个 mirror 正好都填反了,
//    触点水平翻转、垂直不翻,落点和手指完全对不上 —— 现象是"点哪都没反应",
//    很容易误判成硬件坏了。

#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_check.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>

static const char* TAG = "tdeck_touch";

esp_lcd_touch_handle_t tdeck_touch_init(void)
{
    i2c_master_bus_handle_t bus = tdeck_i2c_bus();
    if (!bus) { ESP_LOGE(TAG, "I2C 总线还没建好"); return NULL; }

    const uint8_t addrs[] = { ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                              ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP };
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(bus, addrs[i], 100) == ESP_OK) { addr = addrs[i]; break; }
    }
    if (!addr) { ESP_LOGW(TAG, "没探到 GT911(0x5D / 0x14 都没响应)"); return NULL; }
    ESP_LOGI(TAG, "GT911 在 0x%02X", addr);

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;

    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "触摸 panel io 创建失败");
        return NULL;
    }

    // ⚠️ x_max / y_max 描述的是【GT911 原始坐标空间】,不是显示分辨率。
    // 因为镜像是在交换之前做的(见下),用的是原始轴的量程。
    // T-Deck 的 GT911 原生是 240(x) x 320(y) 竖向。
    // 填成 320/240 的话,y = 240 - raw_y 在 raw_y>240 时对 uint16_t 下溢成 6 万多,
    // 坐标直接乱掉 —— 而且不报任何错。
    esp_lcd_touch_config_t cfg = {
        .x_max        = 240,
        .y_max        = 320,
        .rst_gpio_num = GPIO_NUM_NC,          // T-Deck 没引出触摸复位脚
        // ⚠️ 这里【必须】不给中断引脚。
        //
        // esp_lvgl_port 的 lvgl_port_add_touch() 里有这么一段:
        //     if (int_gpio_num != GPIO_NUM_NC) lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
        // 一旦配了 INT,输入设备就变成【事件模式】—— LVGL 不再主动轮询,
        // 只在中断回调里调 lv_indev_read()。中断链没打通的话,LVGL 一次都不会读,
        // 表现就是"触摸完全没反应",而直连轮询却能读到数据。
        //
        // 这个坑极难查:I2C 正常、芯片正常上报、坐标变换也对,唯独 LVGL 收不到。
        // 走轮询模式代价只是多几次 I2C 读,对这个场景完全无所谓。
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        // ⚠️ esp_lcd_touch.c 的变换顺序是【先镜像,后交换】:
        //     if (mirror_x) x = x_max - x;    // 作用在 raw_x 上
        //     if (mirror_y) y = y_max - y;    // 作用在 raw_y 上
        //     if (swap_xy)  swap(x, y);
        // 所以 mirror_x 管的是【最终的显示 Y】,mirror_y 管的是【显示 X】——
        // 和直觉正好互换。我之前按"mirror_x 管水平"去翻,当然没效果。
        //
        // 真机四角实测(GT911 原始值):
        //     左上 raw(181, 41)   右上 raw(186,282)
        //     右下 raw( 24,291)   左下 raw( 31, 31)
        //   沿顶边走 raw_y 41→282  ⇒ 显示X = raw_y,同向  ⇒ mirror_y = 0
        //   沿右边走 raw_x 186→24  ⇒ 显示Y = 240 - raw_x ⇒ mirror_x = 1
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };

    esp_lcd_touch_handle_t tp = NULL;
    if (esp_lcd_touch_new_i2c_gt911(io, &cfg, &tp) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 初始化失败");
        return NULL;
    }
    ESP_LOGI(TAG, "触摸就绪 %dx%d", TDECK_LCD_H_RES, TDECK_LCD_V_RES);
    return tp;
}
