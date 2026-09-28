// jpeg_size.h — 从 JPEG 字节流里读出真实宽高
//
// 为什么必须读、不能想当然:lv_image_dsc_t 的 header.w/h 是【我们告诉 LVGL 的】,
// 填错了 LVGL 就按错的尺寸去画 —— 解码器解出来的行数对不上,画面要么错位要么裁掉,
// 而且不报任何错。服务端就算承诺"卡片都是 320x240",也不值得拿这个赌。
#pragma once
#include <stdint.h>

namespace tdeck {

// 扫 SOFn 段取宽高。认不出返回 false,*w/*h 不动(调用方保留自己的兜底值)。
inline bool jpeg_size(const uint8_t* d, int len, int* w, int* h)
{
    int i = 2;                                   // 跳过 SOI
    while (i + 9 < len) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        int seg = (d[i + 2] << 8) | d[i + 3];
        // SOFn:C0~CF,但 C4(DHT)/C8/CC 不是
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return *w > 0 && *h > 0;
        }
        if (seg <= 0) return false;
        i += 2 + seg;
    }
    return false;
}

}  // namespace tdeck
