#pragma once
#include <lvgl.h>
namespace tdeck {
// 中文字体。开机时从内嵌的二进制字体建一次,全局共用。
// 没建起来时返回 nullptr,调用方回落到 Montserrat(只有 ASCII)。
void        fonts_init();
const lv_font_t* font_cjk();       // 20px,约 9000 字
// 16px 的同族字体。列表里的歌名/歌手这类次要信息用它 ——
// 20px 在 320x240 上当正文太大了,一行放不下几个字。
// 字符集比 20px 那套小(常用字),生僻字查不到会自动回落。
const lv_font_t* font_cjk_small();
}
