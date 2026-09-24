#ifndef _XZ_DISPLAY_H
#define _XZ_DISPLAY_H

#include "display.h"
#include "xiaozhi_core.h"

// 小智内核的 Display 实现 —— 它【不画任何东西】,只把内核想说的话转成回调。
//
// 为什么不用上游的 LcdDisplay:
// 上游 LcdDisplay::SetupUI() 把状态栏、表情、字幕全挂在 lv_screen_active() 上。
// 在 launcher 里这是灾难:一旦切到别的 app(lv_screen_load_anim),小智的控件树
// 就跟着旧 screen 走,被删掉之后 C++ 侧还握着那些 lv_obj_t*,
// 下一次 SetChatMessage 就是野指针。ADR-004 要求 UI 建在 lv_layer_top() 上,
// 而且 UI 的样式(军绿、浅色底、grokbot 的脸)也和上游那套完全不同。
//
// 所以这一层只做翻译:内核说"状态变了/表情变了/有新字幕",
// UI 层自己决定怎么画。Lock/Unlock 转发 LVGL 锁,保证回调里能安全动控件。
class XzDisplay : public Display {
public:
    XzDisplay();

    void SetCallbacks(const xz::Callbacks& cb) { cb_ = cb; }

    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void UpdateStatusBar(bool update_all = false) override;

protected:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

private:
    // 状态变化没有独立的回调入口(内核的状态机监听器是 Application 的私有成员),
    // 但每次状态变化都必然走一次 SetStatus —— 见 Application::HandleStateChangedEvent,
    // 每个 case 的第一件事都是 display->SetStatus()。所以在这里搭车检查状态,
    // 变了就补发一次 on_state。比按文案反推状态可靠得多。
    void EmitStateIfChanged();

    xz::Callbacks cb_;
    xz::State     last_state_ = xz::State::Unknown;
};

#endif // _XZ_DISPLAY_H
