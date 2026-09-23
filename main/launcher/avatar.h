// avatar.h — 小智的脸
//
// 同一套实现用在两个地方:首屏左上角的小头像(56px),和小智应用里的大脸(150px)。
// 尺寸参数化,行为完全一致 —— 不然就是两套要各自维护的动画。
//
// 不做成 app 图标(ADR-004:小智常驻后台,不是"打开再关闭"的应用)。
// 图标暗示"点我才响应",活的脸暗示"我一直在听"。

#pragma once

#include <lvgl.h>

namespace tdeck {

class Avatar {
public:
    enum class State {
        Idle,       // 待机:偶尔眨眼、四处看、时不时眯成竖条
        Listening,  // 在听:瞳孔放大发白
        Speaking,   // 在说:更亮的绿
    };

    void create(lv_obj_t* parent, int cx, int cy, int d);
    void destroy();
    void set_state(State s);

private:
    void schedule_all();
    static void on_blink(lv_timer_t* t);
    static void on_gaze(lv_timer_t* t);
    static void on_squint(lv_timer_t* t);      // 变竖条
    static void eye_h_cb(void* var, int32_t v);
    static void eye_w_cb(void* var, int32_t v);
    static void eye_shift_cb(void* var, int32_t v);
    void apply_eye_geom();

    lv_obj_t*   head_   = nullptr;
    lv_obj_t*   eye_l_  = nullptr;
    lv_obj_t*   eye_r_  = nullptr;
    lv_timer_t* t_blink_  = nullptr;
    lv_timer_t* t_gaze_   = nullptr;
    lv_timer_t* t_squint_ = nullptr;

    int   d_ = 0;                  // 头的直径
    int   eye_w_ = 0, eye_h_ = 0;  // 当前眼睛尺寸(动画会改)
    int   base_w_ = 0, base_h_ = 0;
    int   bar_w_ = 0,  bar_h_ = 0; // 竖条形态的目标尺寸
    int   eye_cy_ = 0, eye_dx_ = 0;
    int   gaze_ = 0;
    bool  in_bar_ = false;
    State state_ = State::Idle;
};

}  // namespace tdeck
