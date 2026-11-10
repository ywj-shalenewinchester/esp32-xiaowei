#include "protocol.h"
#include "assets.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::AddTextFontCapabilities(cJSON* root) {
    auto capability = Assets::GetInstance().text_font_capability();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);
    }

    if (!capability.glyph_push) {
        return;
    }
    cJSON* font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
    cJSON_AddItemToObject(root, "text_font", font);
}

/* 上报本机屏幕，供服务端按尺寸出图（AI 画图）。
 *
 * 服务端 helloHandle.py 读的是 features.display：
 *     {"width":466,"height":466,"shape":"round","color":true}
 * 缺这条它就退回自己的 default_screen（466x466 圆屏），尺寸对不上时出图会被
 * 拉伸 / 圆形遮罩错位，所以有屏幕就必须报。
 *
 * shape：本工程没有"面板是不是圆的"这个运行期属性可查，方形面板在我们两块板上
 * 都是圆屏（P4 那块 CO5300 466x466），非方形一律按矩形报。服务端在缺 shape 时
 * 也是这么推断的（core/utils/screen_info.py:81），两种口径一致。
 * color：彩色屏。黑白墨水屏链路服务端已经移除，固定报 true。
 *
 * 拿不到屏幕（无显示后端的板子）就整条不报，让服务端走它自己的兜底。
 */
void Protocol::AddDisplayCapabilities(cJSON* root) {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || display->width() <= 0 || display->height() <= 0) {
        return;
    }

    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsObject(features)) {
        return;
    }

    cJSON* display_info = cJSON_CreateObject();
    cJSON_AddNumberToObject(display_info, "width", display->width());
    cJSON_AddNumberToObject(display_info, "height", display->height());
    cJSON_AddStringToObject(display_info, "shape",
                            display->width() == display->height() ? "round" : "rect");
    cJSON_AddBoolToObject(display_info, "color", true);
    cJSON_AddItemToObject(features, "display", display_info);
#if CONFIG_BOARD_TYPE_WZKJ_ESP32_P4_AI_XIAOWEI_PLUS || \
    CONFIG_BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW
    cJSON_AddBoolToObject(features, "artifact_tasks", true);
#endif

    ESP_LOGI(TAG, "Report display capability: %dx%d %s", display->width(), display->height(),
             display->width() == display->height() ? "round" : "rect");
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ +
                       "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word +
                       "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

void Protocol::SendTextMessage(const std::string& text) {
    // 转义 JSON 字符串中的特殊字符（引号、反斜杠、换行等）
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    // 服务端只认 listen/detect 形式的文本输入（等价于一次 ASR 结果），
    // 发 type:"text" 会被当成未知消息，服务端会直接断开连接。
    std::string message = "{\"session_id\":\"" + session_id_ +
                          "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + escaped +
                          "\"}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
