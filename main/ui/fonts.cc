// fonts.cc — 中文字体
//
// 用【cbin 二进制字体】而不是把字库编成 .c 数组:
//   · 通用中文字库有 7000~9000 个字形,编成 C 源码是 5~17MB 的文本,
//     进版本库极其臃肿,每次编译还要重新 parse 一遍
//   · 二进制格式 1.7MB,EMBED_FILES 原样嵌进固件,运行时 cbin_font_create
//     一次建好,不占编译时间
//
// 选择从【内存】加载而不是从文件系统:省掉挂 FATFS 和注册 LVGL 文件系统驱动
// 两层依赖。代价是这 1.7MB 占 app 分区而不是 storage 分区 —— app 分区有 6MB,
// 目前用了不到 2MB,放得下。
//
// cbin_font.c/h 是从 78/xiaozhi-fonts 1.6.0 抠出来的本地副本,没有引整个组件:
// 那个组件用 file(GLOB src/*.c) 会把 30MB 的字体源码全部编进来。
// 2.0.0 版的加载器和这个 cbin 文件不兼容(实测渲染出错位的碎片)。
//
// 字库来自 parent-master 的 stackchan 资源:font_puhui_common_20_4,
// 9291 个字形(含 GB2312 全部 6763 汉字 + 拼音声调 + 中文标点 + 全角),20px 4bpp。
// 对比试过的两条弯路:78/xiaozhi-fonts 的 basic 版只有 801 字形(只够小智自己
// UI 的固定用字,歌名照样方框),完整版是 5MB 的 .c 源码(进版本库太臃肿)。

#include "fonts.h"
#include "cbin_font.h"
#include <esp_log.h>

extern "C" const uint8_t puhui_start[] asm("_binary_font_puhui_common_20_4_bin_start");
extern "C" const uint8_t puhui_end[]   asm("_binary_font_puhui_common_20_4_bin_end");
extern "C" const uint8_t puhui16_start[] asm("_binary_font_puhui_common_16_4_bin_start");
extern "C" const uint8_t puhui16_end[]   asm("_binary_font_puhui_common_16_4_bin_end");

namespace tdeck {
namespace {
const char* TAG = "fonts";
lv_font_t* s_cjk = nullptr;
lv_font_t* s_cjk_s = nullptr;
}

void fonts_init()
{
    if (s_cjk) return;
    uint32_t sz = (uint32_t)(puhui_end - puhui_start);
    // cbin 格式,不是 LVGL 标准 binfont —— 后者的 lv_binfont_create_from_buffer
    // 读这个文件会直接失败(实测)。cbin_font_create 来自 78/xiaozhi-fonts。
    s_cjk = cbin_font_create((uint8_t*)puhui_start);

    // 自检:cbin 是把序列化数据直接映射成 lv_font_fmt_txt_dsc_t 的,
    // 而这个结构在 LVGL 小版本之间会变布局。版本对不上时 cbin_font_create
    // 仍然【返回非空】,只是查任何字形都落空 —— 屏幕上整片方框,连 ASCII 都是。
    // 所以不能只看返回值,要真查一个已知字形。
    //
    // 这个字库是在 LVGL 9.4 下用 78 fork 的 lv_font_conv 生成的,我们跑 9.5,
    // 实测就是全方框。留着这段自检:哪天把版本对齐了,它会自己开始工作。
    if (s_cjk) {
        lv_font_glyph_dsc_t g = {};
        // 用【汉字】验,不能用 'A'。
        // 这个字库是结构体裸 dump,和 LVGL 的 lv_font_fmt_txt_dsc_t 布局绑死。
        // 布局对不上的时候,前几张 cmap(ASCII 那几张)往往还能歪打正着解出来,
        // 'A' 照样有 box_w —— 于是自检通过,而真正要的汉字一个都画不出来。
        // U+4F60「你」落在第 19 张 cmap 上,能查到它才算这个字库是真的可用。
        bool ok = lv_font_get_glyph_dsc(s_cjk, &g, 0x4F60, 0) && g.box_w > 0;
        if (!ok) {
            ESP_LOGW(TAG, "中文字体查不到字形(LVGL 版本与字库不匹配),回落 Montserrat");
            s_cjk = nullptr;
        } else {
            ESP_LOGI(TAG, "中文字体已加载(%u 字节)", (unsigned)sz);
        }
    } else {
        ESP_LOGE(TAG, "中文字体加载失败(%u 字节)", (unsigned)sz);
    }

    // ── 16px 小号 ────────────────────────────────────────
    // 列表里的歌名/歌手用它。20px 当正文在 320 宽的屏上一行放不下几个字。
    // 同样要用汉字自检 —— 字符集比 20px 那套小,而且 LARGE 布局是否一致
    // 只能实测,不能靠文件大小推断。查不到就返回空,调用方回落到 20px。
    {
        uint32_t sz16 = (uint32_t)(puhui16_end - puhui16_start);
        s_cjk_s = cbin_font_create((uint8_t*)puhui16_start);
        if (s_cjk_s) {
            lv_font_glyph_dsc_t g = {};
            if (lv_font_get_glyph_dsc(s_cjk_s, &g, 0x4F60, 0) && g.box_w > 0) {
                ESP_LOGI(TAG, "中文小号字体已加载(%u 字节,行高 %d)",
                         (unsigned)sz16, (int)s_cjk_s->line_height);
            } else {
                ESP_LOGW(TAG, "中文小号字体查不到字形,次要文字回落到 20px");
                s_cjk_s = nullptr;
            }
        }
    }
}

const lv_font_t* font_cjk() { return s_cjk; }
const lv_font_t* font_cjk_small() { return s_cjk_s ? s_cjk_s : s_cjk; }

}  // namespace tdeck
