// avatar.h — 小智的圆形头像
//
// 不做成 app 图标(ADR-004:小智常驻后台,不是一个"打开再关闭"的应用)。
// 它在桌面上是一个始终在场、会眨眼会看你的角色 —— 这个差别是刻意的:
// 图标暗示"点我才响应",活的头像暗示"我一直在听"。

#pragma once

#include <lvgl.h>

namespace tdeck {

class Avatar {
public:
    enum class State {
        Idle,       // 待机:偶尔眨眼、四处看
        Listening,  // 听你说话:瞳孔放大、眼神聚焦
        Speaking,   // 说话:轻微起伏
    };

    // 在 parent 上建头像,圆心 (cx, cy),直径 d
    void create(lv_obj_t* parent, int cx, int cy, int d);
    void destroy();
    void set_state(State s);

private:
    void schedule_blink();
    void schedule_gaze();
    static void on_blink_timer(lv_timer_t* t);
    static void on_gaze_timer(lv_timer_t* t);
    static void eye_height_cb(void* var, int32_t v);
    static void eye_shift_cb(void* var, int32_t v);

    lv_obj_t*   head_    = nullptr;
    lv_obj_t*   eye_l_   = nullptr;
    lv_obj_t*   eye_r_   = nullptr;
    lv_timer_t* blink_t_ = nullptr;
    lv_timer_t* gaze_t_  = nullptr;

    int   eye_w_ = 0, eye_h_ = 0;   // 睁眼时的尺寸
    int   eye_cy_ = 0;              // 眼睛垂直中心(相对 head_)
    int   eye_dx_ = 0;              // 左右眼中心到头像中心的水平距离
    int   gaze_   = 0;              // 当前视线偏移
    State state_  = State::Idle;
};

}  // namespace tdeck
