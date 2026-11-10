#include "board.h"
#include "wifi_board.h"
#include "config.h"
#include "sy6206.h"
#include "low_ui.h"
#include "robot_face.h"
#include "assets/lang_config.h"

#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "backlight.h"
#include "led.h"
#include "button.h"
#include "application.h"
#include "mcp_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"

#include <cJSON.h>
#include <cstring>
#include <memory>

#define TAG "WzkjBoard"

// ST77916 初始化命令集（参考 taiji-pi-s3 标准 QSPI 模式）
static const st77916_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

// 
/**
 * @class WzkjLcdDisplay
 * @brief 继承自SpiLcdDisplay的圆屏显示类，用于处理360x360分辨率的圆屏显示逻辑
 */
class WzkjLcdDisplay : public SpiLcdDisplay {
public:
    // 使用父类构造函数
    using SpiLcdDisplay::SpiLcdDisplay;

    /**
     * @brief 设置用户界面布局
     * @override 重写父类的SetupUI方法，针对圆屏进行特殊布局调整
     */
    void SetupUI() override {
        // 首先调用父类的UI设置方法
        SpiLcdDisplay::SetupUI();
        // 获取显示锁，确保UI操作线程安全
        DisplayLockGuard lock(this);

        // 基类的 container_ 是 400x400，比 360x360 的屏大一圈。LVGL 会在屏幕的
        // DRAW_POST 阶段把屏幕自己的滚动条画在所有子控件之上，于是脸的两侧/底部
        // 会出现滑条。这里把屏幕固定住，并强制纯黑背景。
        lv_obj_t* screen = lv_screen_active();
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        // 主界面只显示全屏机器人表情：文字类控件全部隐藏。
        // （SetChatMessage / SetEmotion / UpdateStatusBar 下面都改成了空操作，
        //   但仍有几条路径会直接 lv_label_set_text，隐藏父控件最省事。）
        HideWidget(status_bar_);
        HideWidget(top_bar_);
        HideWidget(bottom_bar_);
        HideWidget(emoji_box_);
        HideWidget(chat_message_label_);
        HideWidget(preview_image_);

        face_ = std::make_unique<RobotFace>();
        face_->Create(lv_screen_active(), width_, height_);
        if (face_->ready()) {
            ESP_LOGI(TAG, "Robot face ready (%dx%d)", width_, height_);
        } else {
            ESP_LOGE(TAG, "Robot face unavailable (sprite allocation failed)");
        }

        // 按键快捷菜单挂在 lv_layer_top() 上，天然盖在脸之上
        low_ui_init(this);
    }

    /* ---------- 表情接管所有文字输出 ---------- */

    void SetStatus(const char* status) override {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            speaking_ = (std::strcmp(status, Lang::Strings::SPEAKING) == 0);
            // 服务端在 STT 之后会立刻发 tts start，此时还没真正开口；
            // 真正的“开始说话”是第一条 assistant 字幕（见 SetChatMessage），
            // 所以 SPEAKING 期间保留 Think，其他状态才清。
            if (!speaking_) {
                face_->SetFunctionExpression(FaceExpression::Count);
            }
            face_->SetStatus(status);
        }
    }

    void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            face_->SetEmotion(emotion);
        }
    }

    // 主界面没有字幕：聊天消息、通知、状态栏刷新一律不落地。
    // 但字幕文字是设备端少数能拿到的功能信号，这里只“读”不“显示”。
    void SetChatMessage(const char* role, const char* content) override {
        if (face_ == nullptr || role == nullptr || content == nullptr) {
            return;
        }
        if (std::strcmp(role, "assistant") == 0) {
            if (content[0] != '\0') {
                DisplayLockGuard lock(this);
                // 第一条 assistant 字幕 = 真正开始播报了，思考结束。
                // 只清 Think，别把歌曲播放中的 Music 也清掉。
                if (face_->function() == FaceExpression::Think) {
                    face_->SetFunctionExpression(FaceExpression::Count);
                }
            }
        } else if (std::strcmp(role, "user") == 0 && content[0] != '\0' && !speaking_) {
            // 语音识别出结果 = 用户说完了，正在等大模型/工具回答。
            // speaking_ 期间收到的 STT 多半是扬声器里的歌声/回复被拾音，忽略。
            DisplayLockGuard lock(this);
            face_->SetFunctionExpression(FaceExpression::Think);
        }
    }
    void ClearChatMessages() override {}
    void ShowNotification(const char*, int) override {}
    void ShowNotification(const std::string&, int) override {}
    void UpdateStatusBar(bool) override {}

    void SetPowerSaveMode(bool on) override {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            face_->SetPowerSave(on);
        }
    }

    // 服务端下发的天气报文（application.cc 里 type=="weather"）交给菜单缓存
    void DisplayWeather(const char* json, size_t len) override {
        low_ui_set_weather(json, len);
    }

    /* 深度研究/绘画任务（S3 低配版没有 P4 那套 app 界面）：
       - artifact_task：阶段文字借用底部字幕显示；
       - artifact_download：复用按键菜单的二维码页，直接弹出扫码下载。
       图片本身仍走基类 DisplayImage -> SetScreenImage（带 10 秒自动收起）。 */
    void DisplayArtifactEvent(const char* json, size_t len) override {
        cJSON* root = cJSON_ParseWithLength(json, len);
        if (root == nullptr) {
            return;
        }
        auto field = [root](const char* name) -> const char* {
            cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
            return cJSON_IsString(value) ? value->valuestring : "";
        };
        const char* type = field("type");
        if (std::strcmp(type, "artifact_download") == 0) {
            const char* url = field("url");
            if (!low_ui_show_download_qr(url, std::strlen(url))) {
                ESP_LOGW(TAG, "Invalid artifact download URL");
            }
        } else if (std::strcmp(type, "artifact_task") == 0) {
            const char* text = field("text");
            const char* state = field("state");
            const char* kind = field("kind");
            if (std::strcmp(state, "queued") == 0 || std::strcmp(state, "running") == 0) {
                /* 新任务开始：收起上一次留下的下载码，脸进入“工作中”。 */
                low_ui_hide_download_qr();
                SetFaceBase(FaceExpression::AiLoad);
            } else if (std::strcmp(state, "completed") == 0) {
                SetFaceBase(FaceExpression::Idle);
            } else if (std::strcmp(state, "failed") == 0 || std::strcmp(state, "error") == 0) {
                SetFaceBase(FaceExpression::Alert);
            }
            if (std::strcmp(state, "completed") == 0 && std::strcmp(kind, "report") == 0) {
                text = "报告已生成，说“下载报告”获取 PDF";
            }
            if (text[0] != '\0') {
                // 进度显示在底部字幕；TTS 播报会再覆盖一次，属于预期。
                SetChatMessage("system", text);
            }
        }
        cJSON_Delete(root);
    }

    /* 新的绘画结果到达时，先收起可能还开着的下载码，再显示整屏图片。 */
    void DisplayArtifactImage(const uint8_t* data, size_t len, const char* id) override {
        (void)id;
        low_ui_hide_download_qr();
        DisplayImage(data, len);
    }

    /* AI 画图的整屏图片：本板没有触摸屏，点不掉。照 Plus 版
       （XiaoweiUiDisplay::StartScreenImageTimer）的做法，显示满 10 秒自动收起。
       基类的 SetScreenImage 是虚函数，这里只是"落地之后补一个计时器"。 */
    void SetScreenImage(std::unique_ptr<LvglImage> image) override {
        LcdDisplay::SetScreenImage(std::move(image));
        const bool shown = HasScreenImage();
        // 整屏图片在脸的下面一层：看图期间先把脸收起来，收起后自动回来。
        {
            DisplayLockGuard lock(this);
            if (face_ != nullptr) {
                face_->SetVisible(!shown);
            }
        }
        if (shown) {
            StartScreenImageTimer();
        } else if (screen_image_timer_ != nullptr) {
            esp_timer_stop(screen_image_timer_);
        }
    }

    // 点击 / 超时收起整屏图片时由基类回调（锁已在手上）。
    void OnScreenImageHidden() override {
        if (face_ != nullptr) {
            face_->SetVisible(true);
        }
    }

private:
    static constexpr uint32_t kScreenImageDurationMs = 10000;
    esp_timer_handle_t screen_image_timer_ = nullptr;
    std::unique_ptr<RobotFace> face_;
    // 当前是否处于 SPEAKING 状态（用于避免把播放中的歌声当成用户说话）。
    bool speaking_ = false;

    static void HideWidget(lv_obj_t* obj) {
        if (obj != nullptr) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 服务端任务事件把脸切到"工作中/结束"（没有字幕可用）。
    void SetFaceBase(FaceExpression expression) {
        DisplayLockGuard lock(this);
        if (face_ != nullptr) {
            face_->SetBaseExpression(expression);
        }
    }

    void StartScreenImageTimer() {
        if (screen_image_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback =
                    [](void* arg) { static_cast<WzkjLcdDisplay*>(arg)->OnScreenImageTimeout(); },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "low_screen_img",
                .skip_unhandled_events = false,
            };
            ESP_ERROR_CHECK(esp_timer_create(&args, &screen_image_timer_));
        }
        // 上一张还没到点又来一张：重新计时，别让它把新图提前收走。
        esp_timer_stop(screen_image_timer_);
        ESP_ERROR_CHECK(
            esp_timer_start_once(screen_image_timer_, (uint64_t)kScreenImageDurationMs * 1000));
    }

    // 跑在 esp_timer 任务上，自己拿 LVGL 锁（lvgl_port_lock 可重入）。
    void OnScreenImageTimeout() {
        DisplayLockGuard lock(this);
        if (!HasScreenImage()) {
            return;
        }
        ESP_LOGI(TAG, "AI image timeout, hiding");
        HideScreenImage();
    }
};

/**
 * @class WzkjBoard
 * @brief 继承自WifiBoard的WzkjBoard类，实现了特定硬件平台的功能
 */
class WzkjBoard : public WifiBoard {
private:
    // I2C总线句柄，用于音频设备和电源管理芯片的通信
    i2c_master_bus_handle_t audio_i2c_bus_;
    i2c_master_bus_handle_t pmic_i2c_bus_;
    // 电源管理芯片指针
    Sy6206                 *pmic_;
    // PMIC I2C 看门狗喂狗定时器
    esp_timer_handle_t       pmic_wdt_timer_ = nullptr;
    // 按键对象，包括启动按钮和电源按钮
    Button                  boot_button_;
    Button                  pwr_button_;  // 对应 QON 按键
    // 显示设备相关句柄
    LcdDisplay             *display_ = nullptr;
    esp_lcd_panel_handle_t  panel_ = nullptr;  // 关机时用于关闭显示

    // 1. 初始化 音频 I2C 总线
    void InitializeAudioI2c() {
        ESP_LOGI(TAG, "Initialize Audio I2C: SDA=%d SCL=%d port=%d",
                 AUDIO_I2C_SDA_PIN, AUDIO_I2C_SCL_PIN, AUDIO_I2C_PORT);

        i2c_master_bus_config_t audio_bus_cfg = {
            .i2c_port = AUDIO_I2C_PORT,
            .sda_io_num = (gpio_num_t)AUDIO_I2C_SDA_PIN,
            .scl_io_num = (gpio_num_t)AUDIO_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&audio_bus_cfg, &audio_i2c_bus_));   
    }

    // 2. 初始化 PMIC I2C 总线和 SY6206
    void InitializePmic() {
        ESP_LOGI(TAG, "Initialize PMIC I2C: SDA=%d SCL=%d port=%d",
                 PMIC_I2C_SDA_PIN, PMIC_I2C_SCL_PIN, PMIC_I2C_PORT);

        i2c_master_bus_config_t pmic_bus_cfg = {
            .i2c_port = PMIC_I2C_PORT,
            .sda_io_num = (gpio_num_t)PMIC_I2C_SDA_PIN,
            .scl_io_num = (gpio_num_t)PMIC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&pmic_bus_cfg, &pmic_i2c_bus_));

        pmic_ = new Sy6206(pmic_i2c_bus_, SY6206_ADDR);
        pmic_->SetHardwareChargeEnable(true);
        pmic_->SetChargeVoltageMv(4200);  // 4.2V 充满电压
        pmic_->SetChargeCurrentMa(512);   // 512mA 充电电流
        pmic_->SetChargeEnable(true);
        // NTC 引脚悬空, 不禁用温度检测芯片会拒绝充电 (实测验证)
        pmic_->DisableNtcCheck();
        pmic_->EnableAdc(true);           // 连续模式 ADC

        // PMIC I2C 看门狗超时会自动停止充电, 周期性喂狗;
        // 同时轮询中断标志 (REG20/REG22 为读清零, 读取即清中断并恢复 INT 引脚),
        // 解析触发原因: 插拔电源 / QON 按键 / 看门狗超时 / 过温 / 过压等
        const esp_timer_create_args_t wdt_args = {
            .callback = [](void *arg) {
                Sy6206 *pmic = static_cast<Sy6206 *>(arg);
                pmic->FeedWatchdog();    // 核心逻辑一: 定期喂狗
                pmic->HandleInterrupt(); // 核心逻辑二: 读中断原因并解析
            },
            .arg = pmic_,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pmic_wdt",
        };
        ESP_ERROR_CHECK(esp_timer_create(&wdt_args, &pmic_wdt_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pmic_wdt_timer_, 2 * 1000 * 1000)); // 2s

        uint8_t chip_id = 0, rev = 0;
        if (pmic_->ReadChipId(&chip_id, &rev) == ESP_OK) {
            ESP_LOGI(TAG, "SY6206 ready: chip_id=%u rev=%u", chip_id, rev);
        }
    }

    // 3. 初始化 QSPI 总线和 ST77916 LCD 面板
    void InitializeLcdDisplay() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        const spi_bus_config_t bus_config = WZKJ_ST77916_PANEL_BUS_QSPI_CONFIG(
            LCD_QSPI_CLK_PIN, LCD_QSPI_D0_PIN, LCD_QSPI_D1_PIN,
            LCD_QSPI_D2_PIN, LCD_QSPI_D3_PIN,
            DISPLAY_WIDTH * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS_PIN;
        io_config.dc_gpio_num = GPIO_NUM_NC;  // QSPI 不需要独立 DC
        io_config.spi_mode = 0;
        /* ST77916 的 QSPI 写入支持到 80MHz，原来 40MHz 时整屏 360×360
           要 52ms 才能刷完，滑块/翻页都看得出卡顿。80MHz 直接砍半（约 26ms）。
           万一屏线偏长导致花屏，往回降到 60MHz（或 40MHz）。 */
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST,
                                                 &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver");
        st77916_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;  // RGB565
        panel_config.reset_gpio_num = LCD_RST_PIN;
        panel_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel_));
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_disp_on_off(panel_, true);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new WzkjLcdDisplay(panel_io, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                      DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 4. 初始化按键逻辑
    void InitializeButtons() {
        // BOOT 短按：低配版没有触摸屏，改用“手表式”按键菜单。
        //   菜单没开 → 从顶部滑下快捷菜单
        //   菜单开着 → 在 音量 / 亮度 / 二维码 / 天气 / 关闭 之间循环选择
        boot_button_.OnClick([]() {
            if (!low_ui_on_select()) {
                low_ui_open_menu();
            }
        });

        // BOOT 长按：原来的“唤醒助手 / 开始或打断对话”挪到这里，避免和短按冲突。
        //   启动期 → 进入配网
        //   运行期 → ToggleChatState()，空闲时开始对话，speaking 时打断当前回复
        //   菜单开着时不动，否则长按会被误当成选择（菜单里选“关闭”即可退出）
        boot_button_.OnLongPress([this]() {
            if (low_ui_is_open()) {
                return;
            }
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // QON 短按：确认菜单里当前高亮的那一项（菜单没开时无操作）
        pwr_button_.OnClick([]() { low_ui_on_confirm(); });

        // QON 长按：进入深度睡眠，通过 QON 重新唤醒
        pwr_button_.OnLongPress([this]() {
            // 关机流程由主任务执行，避免在按键回调里阻塞
            Application::GetInstance().Schedule([this]() {
                ESP_LOGI(TAG, "Shutting down");

                // v9 的关机动画：闭眼 → 脸淡出 → logo 亮起，约 3.2 s。
                if (display_ != nullptr) {
                    display_->SetStatus("关机");
                }
                vTaskDelay(pdMS_TO_TICKS(3300));

                GetBacklight()->SetBrightness(0);
                // 关闭显示，避免睡眠期间残留
                if (panel_ != nullptr) {
                    esp_lcd_panel_disp_on_off(panel_, false);
                }

                // QON 是 RTC GPIO，睡眠期间保持上拉，避免悬空误唤醒
                ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup((gpio_num_t)PMIC_QON_BUTTON_PIN, 0));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en((gpio_num_t)PMIC_QON_BUTTON_PIN));
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis((gpio_num_t)PMIC_QON_BUTTON_PIN));

                esp_deep_sleep_start();
            });
        });
    }

    // 5. 注册 MCP 工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网",
                           PropertyList(),
                           [this](const PropertyList &) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });
    }

public:
    WzkjBoard() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(PMIC_QON_BUTTON_PIN) {
        InitializePmic();        // 必须最先通电
        InitializeAudioI2c();    // 初始化音频总线
        InitializeLcdDisplay();  // 初始化屏幕
        InitializeButtons();     // 注册按键
        InitializeTools();       // 注册配网等工具服务
        GetBacklight()->RestoreBrightness();    // 恢复用户上次设置的亮度
    }

    
    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            (void *)audio_i2c_bus_,
            AUDIO_I2C_PORT,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            (gpio_num_t)AUDIO_ES8311_MCLK_PIN,
            (gpio_num_t)AUDIO_ES8311_SCLK_PIN,
            (gpio_num_t)AUDIO_ES8311_LRCK_PIN,
            (gpio_num_t)AUDIO_ES8311_DAC_DATA_PIN,
            (gpio_num_t)AUDIO_ES8311_ADC_DATA_PIN,
            (gpio_num_t)AUDIO_NS4150B_CTRL_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Led *GetLed() override {
        static GpioLed led(R_LED_PIN, R_LED_ACTIVE_LEVEL, R_LED_BLINK_INTERVAL_MS);
        return &led;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (!pmic_) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }

        // 内阻补偿后的开路电压 + 放电曲线查表,
        // 避免充电时裸 VBAT 读数虚高导致插拔电量跳变
        Sy6206::BatteryState state;
        if (pmic_->GetBatteryState(state) == ESP_OK) {
            level = state.percentage;
            charging = state.is_charging;
            discharging = !charging;
            return true;
        }

        level = 0;
        charging = false;
        discharging = false;
        return false;
    }
};

DECLARE_BOARD(WzkjBoard);
