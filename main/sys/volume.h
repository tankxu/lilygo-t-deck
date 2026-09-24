#pragma once
#include <stdint.h>

// 系统级音量。
//
// 放在系统层而不是音乐 app 里,是因为音量是【全局】的:小智在说话、自检在
// 放测试音、以后的 bilibili 在放视频,按 O/I 都得能调。做在 app 里的话每个
// app 都要重写一遍,而且互相看不见对方的设置值。
//
// 键位:BBQ10 键盘上 I 和 O 的副标就是 - 和 +(alt 层),所以约定
//   I → 调小    O → 调大
// 见 ADR-006。
namespace tdeck::volume {

// 从 NVS 读回上次的值并下发给功放。要在 tdeck_audio_init 之前调也没关系 ——
// 音量是纯软件的,BSP 只是存着,下次写 PCM 时才用。
void init();

int  get();                 // 0..100
void set(int percent);      // 夹到 0..100,存 NVS,弹 HUD

// delta 为正调大。O/I 走这个。
void step(int delta);

// 音量被别处改了的通知。音乐 app 用它同步自己的滑块 ——
// 否则在播放页按 O/I,滑块不动,看起来像没生效。
using Listener = void (*)(int percent, void* user);
void subscribe(Listener cb, void* user);
void unsubscribe(void* user);

}  // namespace tdeck::volume
