/*
 * 小未 LCD UI —— 硬件适配层（目标工程侧）
 *
 * ui/ 这套代码原本跑在 lcd 工程里，硬件访问是它自己那套 main.cpp / *_test.c 提供的。
 * 本工程把这些能力接到自己的板级驱动与框架抽象上，接口声明见 xiaowei_ui_hw.h。
 *
 * 另外还负责覆盖 xiaowei_ui_port.c 里那几个钩子的实现：
 *   xiaowei_get_brightness / xiaowei_set_brightness   —— 那边是**裸 extern，没有
 *       weak 兜底**，不提供就在这里链接失败；这是本文件存在的最硬理由。
 *   xiaowei_audio_get_volume / set_volume / xiaowei_ui_show_notification
 *       —— 那边有 weak 空实现，本文件的强符号自动覆盖。
 * 日期/时间、on_enter/on_exit_app 由 xiaowei_ui_port.c 自己实现，这里不管。
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "backlight.h"
#include "audio_codec.h"
#include "display.h"
/* Application 一路带出 AudioService / DeviceState / FreeRTOS 头。
 * audio 页要用 GetAudioService().EnableAudioTesting() 和 GetDeviceState()。 */
#include "application.h"

#include "xiaowei_ui_hw.h"

static const char *TAG = "XwUiHw";

/* ==========================================================================
 *  亮度
 *
 *  走框架的 Backlight 抽象，不直接戳 CO5300 面板命令：基类已经带了渐变定时器
 *  和 Settings 持久化，直接发面板命令等于把这些绕过去，两套状态还会打架。
 *  目标板的 Co5300Backlight 就是这个抽象在 CO5300 上的实现。
 * ========================================================================== */
extern "C" int xiaowei_get_brightness(void) {
    auto *bl = Board::GetInstance().GetBacklight();
    return bl ? static_cast<int>(bl->brightness()) : 0;
}

extern "C" bool xiaowei_set_brightness(int value) {
    auto *bl = Board::GetInstance().GetBacklight();
    if (bl == nullptr) {
        return false;
    }
    // permanent=false：设置页的滑条拖动会连续调用，每步都写 NVS 既慢又费寿命。
    // 亮度最终值由框架自己的持久化路径负责落盘。
    bl->SetBrightness(static_cast<uint8_t>(std::clamp(value, 0, 100)), false);
    return true;
}

/* ==========================================================================
 *  音量
 * ========================================================================== */
extern "C" int xiaowei_audio_get_volume(void) {
    auto *codec = Board::GetInstance().GetAudioCodec();
    return codec ? codec->output_volume() : 0;
}

extern "C" bool xiaowei_audio_set_volume(int value) {
    auto *codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return false;
    }
    codec->SetOutputVolume(std::clamp(value, 0, 100));
    return true;
}

/* ==========================================================================
 *  通知（快捷面板等 UI 元素弹的提示）
 * ========================================================================== */
extern "C" void xiaowei_ui_show_notification(const char *title, const char *message) {
    if (title == nullptr && message == nullptr) {
        return;
    }
    // 拼成一行交给 display：UI 的 notification 接口在 xiaozhi 侧是单条字幕，
    // 这里把标题和正文合起来，避免只显示一半。
    std::string text;
    if (title != nullptr) {
        text = title;
    }
    if (message != nullptr) {
        if (!text.empty()) {
            text += ": ";
        }
        text += message;
    }
    Board::GetInstance().GetDisplay()->ShowNotification(text);
}

/* ==========================================================================
 *  IMU
 *
 *  xiaowei_imu_get() 不在这里实现 —— 器件对象归板级持有，转发没有意义。
 *  实现在板级 xiaowei_plus.cc（和 camera 那组一样，链接期直接解析）。
 * ========================================================================== */

/* ==========================================================================
 *  触觉马达
 *
 *  xiaowei_haptic_play_wave / is_playing / set_intensity 同样由板级
 *  xiaowei_plus.cc 定义：板级持有 Aw86937 对象，这里不转发。
 *  模式映射和测试序列属于 UI 语义，在 apps/legacy/legacy_app.c。
 * ========================================================================== */

/* ==========================================================================
 *  录音回放 —— 走框架自己的录音测试通路
 *
 *  为什么必须走 AudioService::EnableAudioTesting 而不是让页面自己读麦克风：
 *  AudioInputTask 是**唯一**的麦克风读取者，而它的 testing 分支在喂 AFE 之前
 *  就 `continue` 了（audio_service.cc:267-289），所以 TESTING 位一置上，
 *  唤醒词自然停喂 —— 两边不抢麦克风。反过来，页面自己开任务读同一个 rx_handle_
 *  会瓜分数据、把唤醒词饿死，而且 NoAudioCodecSimplexPdm::Read 内部是
 *  portMAX_DELAY 且**不持** data_if_mutex_，并发读没有任何保护。
 *
 *  设备状态机不允许 Idle -> kDeviceStateAudioTesting（device_state_machine.cc:71-78），
 *  那条 testing 流程绑死在配网模式。所以这里只操作 AudioService，不碰设备状态。
 * ========================================================================== */

namespace {

std::atomic<bool> s_recording{false};

// 这一轮录音的预计结束时刻。框架自己在录满 AUDIO_TESTING_MAX_DURATION_MS 时会调
// EnableAudioTesting(false) 转进回放（audio_service.cc:268-271），我们的标志不会
// 因此自动落下 —— 靠这个时刻把它对上，界面才不会永远停在 RECORDING。
std::atomic<int64_t> s_recording_until_us{0};

// 唤醒词是否被我们摘掉了（没摘就不用装回去）。
std::atomic<bool> s_wake_word_suspended{false};
std::atomic<bool> s_restore_task_running{false};

// 还"在录音"吗。过了预计结束时刻就自动落下 —— 那是框架自己停的。
bool recording_active() {
    if (!s_recording.load()) {
        return false;
    }
    if (esp_timer_get_time() < s_recording_until_us.load()) {
        return true;
    }
    s_recording.store(false);
    return false;
}

/* 等设备真正空闲后把唤醒词装回去。
 *
 * 为什么必须挂起唤醒词：EnableAudioTesting(false) 的瞬间，框架把 testing 队列整个
 * 搬进回放队列，TESTING 位清掉、而 WAKE_WORD 位**还在**。于是 AFE 立刻恢复喂数据，
 * 同时喇叭开始放刚才录的那段 —— 那段里有唤醒词的话，设备会被自己的回放唤醒拽进
 * Listening。本工程 device AEC 是关的，指望不上它把回放消掉。
 *
 * 为什么恢复点不能放在 stop() 里：stop() 之后还有整段回放没放完，这期间 AFE 一直在喂。
 * 所以起一个只在挂起期间存在的任务盯着，等 AudioService 空闲**且**设备回到 Idle
 * 再装回去 —— 后者是为了不在对话/配网中途把唤醒词打开。
 *
 * 用 FreeRTOS 任务而不是 lv_timer/esp_timer：唤醒词的开关跟 LVGL 无关（不该依赖页面
 * 还开着），而 EnableWakeWordDetection(true) 会走 InitializeAudioEngine()，那条路要
 * 建任务/申请内存，放 esp_timer 的默认小栈上跑有溢出风险。 */
void wake_word_restore_task(void *) {
    Application &app = Application::GetInstance();
    while (s_wake_word_suspended.load()) {
        if (app.GetAudioService().IsIdle() && app.GetDeviceState() == kDeviceStateIdle) {
            if (s_wake_word_suspended.exchange(false)) {
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "音频测试已结束且设备空闲，恢复唤醒词");
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_restore_task_running.store(false);
    vTaskDelete(nullptr);
}

// 起一个（且只有一个）等待恢复的任务。可以随便重复调用 —— 页面定时器和
// record_stop() 都会调它，两条路都可能是"最后一个动作"。
void arm_wake_word_restore() {
    if (!s_wake_word_suspended.load()) {
        return;
    }
    bool expected = false;
    if (!s_restore_task_running.compare_exchange_strong(expected, true)) {
        return;  // 已经有一个在等了
    }
    if (xTaskCreate(wake_word_restore_task, "ww_restore", 4096, nullptr, 4, nullptr) != pdPASS) {
        s_restore_task_running.store(false);
        ESP_LOGE(TAG, "唤醒词恢复任务创建失败，唤醒词将保持关闭直到重启");
    }
}

}  // namespace

extern "C" bool xiaowei_audio_record_start(void) {
    Application &app = Application::GetInstance();
    AudioService &audio = app.GetAudioService();

    // 只允许从 Idle 进：正在跟 AI 说话 / 配网 / 升级时插一脚会打断那条流程。
    if (app.GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "设备不在空闲态，忽略录音测试请求");
        return false;
    }
    if (recording_active()) {
        return true;  // 已在录，幂等
    }

    // 唤醒词本来就没开就不用挂起（也就没有"装回去"这一步）。
    if (audio.IsWakeWordRunning()) {
        audio.EnableWakeWordDetection(false);
        s_wake_word_suspended.store(true);
    }

    audio.EnableAudioTesting(true);
    s_recording.store(true);
    s_recording_until_us.store(
        esp_timer_get_time() + static_cast<int64_t>(AUDIO_TESTING_MAX_DURATION_MS) * 1000);
    ESP_LOGI(TAG, "音频测试：开始录音（唤醒词已挂起，上限 %d 秒）",
             AUDIO_TESTING_MAX_DURATION_MS / 1000);
    return true;
}

extern "C" void xiaowei_audio_record_stop(void) {
    // 只在**还在录**的时候调 API。过了框架自己的 10 秒上限之后 testing 已经结束，
    // 再调一次 EnableAudioTesting(false) 会把 audio_decode_queue_ 赋值成已经空掉的
    // testing 队列，等于把还没播完的回放整个丢掉。
    if (recording_active()) {
        s_recording.store(false);
        Application::GetInstance().GetAudioService().EnableAudioTesting(false);
        ESP_LOGI(TAG, "音频测试：停止录音，转入回放");
    }
    // 回放可能还在继续，恢复唤醒词的事交给后台任务等空闲。
    arm_wake_word_restore();
}

extern "C" bool xiaowei_audio_busy(void) {
    // IsIdle() 覆盖了"录完后还在回放"这段：testing 队列空了但 decode 队列还没排干。
    const bool busy = recording_active() ||
                      !Application::GetInstance().GetAudioService().IsIdle();
    if (!busy) {
        // 整轮走完了。这里是个天然的收尾点 —— audio 页每 200ms 调一次本函数，
        // 于是"用户从不按 STOP、直接右滑退出"这条路径也能把唤醒词装回去。
        arm_wake_word_restore();
    }
    return busy;
}

extern "C" bool xiaowei_audio_recording(void) {
    return recording_active();
}

/* ==========================================================================
 *  麦克风电平
 *
 *  实现落在板级目录（xiaowei_plus.cc 末尾），因为电平是 XiaoweiPlusAudioCodec 的
 *  成员，而 UI 层不该 include 板级头文件 —— 那会把 P4 板的类型塞进本来与板型无关的
 *  界面代码里。这里只做一层转发，纯 C 符号，链接期解析。
 * ========================================================================== */
extern "C" void xiaowei_plus_audio_input_levels(int *rms, int *peak);
extern "C" void xiaowei_plus_audio_afe_levels(int *rms, int *peak);

extern "C" void xiaowei_audio_input_levels(int *rms, int *peak) {
    xiaowei_plus_audio_input_levels(rms, peak);
}

extern "C" void xiaowei_audio_afe_levels(int *rms, int *peak) {
    xiaowei_plus_audio_afe_levels(rms, peak);
}

/* ==========================================================================
 *  摄像头
 *
 *  这一组**故意不在这里实现也不转发**：板级目录的 gc2145.cc（GC2145 传感器
 *  + DVP 采集管线）定义的符号名和 xiaowei_ui_hw.h 里的声明完全同名，链接期
 *  直接解析即可。转发层只在两边名字不同时才值得存在 —— 音频电平那两路就是
 *  这种情况（板级符号叫 xiaowei_plus_audio_*），摄像头不是。
 *
 *  所以本文件对摄像头只负责「声明在头里」，没有一个函数体。
 * ========================================================================== */
