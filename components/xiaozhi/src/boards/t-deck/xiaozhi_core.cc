// xiaozhi_core.cc — 内核的启动与唤醒入口
//
// 上游是 app_main() 里 Initialize() + Run(),Run() 永不返回、独占 main task。
// 在这个 OS 里小智只是常驻后台的一员(ADR-004),所以把这两句搬进自己的任务。
// 行为一字不变:Run() 本体是 xEventGroupWaitBits(portMAX_DELAY),空闲不占 CPU。

#include "xiaozhi_core.h"
#include "application.h"
#include "xz_display.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "xz"

XzDisplay* tdeck_xz_display();   // 定义在 tdeck_board.cc

namespace xz {

static bool         s_started = false;
static Callbacks    s_cb;
static TaskHandle_t s_task = nullptr;

void set_callbacks(Callbacks cb) {
    s_cb = std::move(cb);
    tdeck_xz_display()->SetCallbacks(s_cb);
}

static void XiaozhiTask(void*) {
    auto& app = Application::GetInstance();
    app.Initialize();   // 音频服务 + MCP + 网络回调,内部会起 3 个音频任务
    app.Run();          // 事件循环,永不返回
}

bool start() {
    if (s_started) return false;

    // 栈必须在内部 RAM(FreeRTOS 任务栈不能放 PSRAM),而内部 RAM 是这块板子上
    // 最紧的资源,所以开机时把余量打出来 —— 起不来的时候第一眼就能看见原因。
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // 16KB。不是拍脑袋:这个任务里会跑 OTA 检查和 WebSocket 握手,都是 TLS,
    // mbedtls 握手峰值实测 ~10KB。同一个坑刚在 net 任务上炸过一次 ——
    // 6144 的栈溢出了 4KB,距离超过了 FreeRTOS 的金丝雀,所以【不报 stack overflow】,
    // 只是把隔壁任务的数据结构写坏,表现成 LVGL 随机崩。宁可多给 4KB。
    TaskHandle_t h = nullptr;
    auto ok = xTaskCreate([](void* a) { XiaozhiTask(a); }, "xiaozhi_main",
                          16 * 1024, nullptr, 5, &h);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "小智主任务建不起来,内部 RAM 只剩 %u 字节", (unsigned)before);
        return false;
    }
    s_started = true;
    s_task = h;
    ESP_LOGI(TAG, "小智内核启动,内部 RAM %u → %u 字节",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // bring-up 期间每 30 秒报一次栈余量和内部 RAM。
    // ⚠️ uxTaskGetStackHighWaterMark 在 ESP-IDF 里返回的是【字节】,
    // 不是原版 FreeRTOS 文档写的【字】—— 再乘 sizeof(StackType_t) 会虚报 4 倍,
    // 把"只剩 2KB"读成"还有 8KB",正好在最危险的时候给人安全感。
    esp_timer_create_args_t args = {
        .callback = [](void*) {
            if (!s_task) return;
            // 音频那三个任务是 AudioService 私有的,但 FreeRTOS 可以按名字要句柄 ——
            // 比为了打一行日志去改内核的头文件干净。opus_codec 是大头(24KB 栈),
            // 内部 RAM 紧张的时候第一个该问的就是它到底用了多少。
            auto hw = [](const char* n) -> unsigned {
                TaskHandle_t h = xTaskGetHandle(n);
                return h ? (unsigned)uxTaskGetStackHighWaterMark(h) : 0;
            };
            ESP_LOGI(TAG, "栈余量 主%u 输入%u 输出%u opus%u 字节;内部 RAM %u(历史最低 %u)",
                     (unsigned)uxTaskGetStackHighWaterMark(s_task),
                     hw("audio_input"), hw("audio_output"), hw("opus_codec"),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "xz_watch",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t t = nullptr;
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_periodic(t, 30ULL * 1000000);
    return true;
}

bool started() { return s_started; }

State state() {
    if (!s_started) return State::Unknown;
    switch (Application::GetInstance().GetDeviceState()) {
    case kDeviceStateStarting:        return State::Starting;
    case kDeviceStateWifiConfiguring: return State::WifiConfiguring;
    case kDeviceStateIdle:            return State::Idle;
    case kDeviceStateConnecting:      return State::Connecting;
    case kDeviceStateListening:       return State::Listening;
    case kDeviceStateSpeaking:        return State::Speaking;
    case kDeviceStateUpgrading:       return State::Upgrading;
    case kDeviceStateActivating:      return State::Activating;
    case kDeviceStateAudioTesting:    return State::AudioTesting;
    case kDeviceStateFatalError:      return State::FatalError;
    default:                          return State::Unknown;
    }
}

const char* state_text(State s) {
    switch (s) {
    case State::Starting:        return "启动中";
    case State::WifiConfiguring: return "配网中";
    case State::Idle:            return "待命";
    case State::Connecting:      return "连接中";
    case State::Listening:       return "聆听中";
    case State::Speaking:        return "说话中";
    case State::Upgrading:       return "升级中";
    case State::Activating:      return "激活中";
    case State::AudioTesting:    return "音频自检";
    case State::FatalError:      return "出错了";
    default:                     return "";
    }
}

// 下面四个都只是往 EventGroup 置位(上游注释明写 thread-safe),
// 所以可以直接在 LVGL 任务或输入任务里调,不会阻塞界面。
void toggle_chat() {
    if (!s_started) return;
    Application::GetInstance().ToggleChatState();
}

void start_listening() {
    if (!s_started) return;
    Application::GetInstance().StartListening();
}

void stop_listening() {
    if (!s_started) return;
    Application::GetInstance().StopListening();
}

void abort_speaking() {
    if (!s_started) return;
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
}

}  // namespace xz
