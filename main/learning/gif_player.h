// gif_player.h — 自己控制刷新区域的 GIF 播放器
//
// 为什么不直接用 lv_gif:它每帧都 lv_obj_invalidate(obj) 让【整幅】失效。
// 笔顺动画是 240x240,画布是 ARGB8888(4 字节/像素,240*240*4 = 230KB),
// 而且这个尺寸必然落在 PSRAM 上 —— 于是每帧都要从 PSRAM 把 230KB 读出来
// 转成 RGB565 重绘一遍。
//
// 实测代价:一轮标称 7.1 秒的笔顺动画,在板子上要跑【18 秒】(2.5 倍),
// 而且把小智的音频一起挤卡(两边都在抢 PSRAM 带宽)。
//
// 而这种动画 41 帧里【只有 1 帧是整幅的】,其余都是增量小块(最小 5x5)——
// gifdec 自己就只渲染变化矩形(gd_render_frame → render_frame_rect),
// 浪费全在"渲染完之后整幅失效"这一步。
//
// 所以这里只改一件事:只让【变化过的那块】失效。
// 取 union(上一帧矩形, 这一帧矩形) —— 上一帧的也要算进去,
// 因为 GIF 的 disposal 会把上一帧那块擦回背景。
//
// ⚠️ 不能去改 managed_components 里的 lv_gif.c:那个目录在 .gitignore 里,
// 补丁提交不了,下次拉组件就没了。所以宁可自己写这一百来行。

#pragma once

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

namespace tdeck {

// 建一个 GIF 播放对象。data 必须在对象活着期间一直有效(内部直接引用,不拷贝)。
// 建不起来返回 nullptr。对象销毁时自动释放解码器和画布。
lv_obj_t* gif_player_create(lv_obj_t* parent, const void* data, size_t len);

// 动画播完(最后一轮结束)时发 LV_EVENT_READY。
// 调用方据此决定还要留多久 —— 固定倒计时对笔画多的字不够用。
// 拿不到总帧数就别猜:不同字的帧数差好几倍。

// 播完了没有。还在放返回 false。
bool gif_player_finished(lv_obj_t* obj);

}  // namespace tdeck
