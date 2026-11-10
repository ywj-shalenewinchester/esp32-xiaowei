#include "xiaowei_ui_display.h"

#include <esp_log.h>

#include <cJSON.h>
#include <cstring>

#include "application.h"
#include "device_state.h"

/* "display/src/ui" 在 main/CMakeLists.txt 的 INCLUDE_DIRS 里（P4 分支），
 * 所以这里可以直接按文件名包含。 */
#include "xiaowei_ui.h"
#include "apps/artifact/artifact_app.h"
#include "apps/camera/photo_store.h"
#include "apps/chat/chat_app.h"
#include "apps/qrcode/qrcode_app.h"
#include "framework/app_manager.h"
#include "framework/quick_panel.h"
#include "lvgl_display/lvgl_theme.h"
#include "ported/weather_screen.h"
#include "shell/standby_ring.h"

static const char* TAG = "XiaoweiUiDisplay";

/* ==========================================================================
 *  聊天页的手势 —— 它是待机环上的一页
 *
 *  聊天页和壁纸页、表情页是同级的三种待机模式（见 shell/standby_ring.h）：
 *      壁纸 --左滑--> 聊天 --左滑--> 表情 --左滑--> 壁纸（循环）
 *  三页上**右滑一律进应用主菜单**，而不是"返回上一页"。
 *
 *  所以这里不再是"右滑返回"，而是交给 standby_ring 决定去哪：
 *  右滑 = 主菜单，左滑 = 下一个模式。
 *  （之前右滑直接 xiaowei_app_back()，在环上会从返回栈弹页 —— 那是"返回"语义，
 *   和"右滑出菜单"打架。）
 * ========================================================================== */

static void chat_gesture_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        standby_ring_next();
    } else if (dir == LV_DIR_RIGHT) {
        standby_ring_show_menu();
    } else if (dir == LV_DIR_BOTTOM) {
        /* 下拉快捷面板。它挂在 lv_layer_top() 上（framework/quick_panel.c），
           三页共用同一个，所以这里只要转发开合。上滑收起不在这里：
           面板一打开就盖满整屏、成为 act_obj，手势由面板自己接。 */
        xiaowei_quick_panel_open();
    }
}

/* 递归挂到整棵子树上。
 *
 * 必须递归，只挂屏幕收不到：基类建出来的 container_ / top_bar_ / status_bar_ /
 * emoji_box_ 全是 lv_obj_create() 出来的**可点击**对象，手指落在上面时 LVGL 把
 * LV_EVENT_GESTURE 发给的是那个被按住的子对象，不是屏幕。
 * （挂在 label 上无害 —— label 默认不可点击，本来也不会成为被按住的对象。） */
static void attach_gesture_recursive(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_add_event_cb(obj, chat_gesture_cb, LV_EVENT_GESTURE, nullptr);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        attach_gesture_recursive(lv_obj_get_child(obj, i));
    }
}

/* ==========================================================================
 *  语音活动时自动切到聊天页
 *
 *  用轮询而不是 AddStateChangeListener：那个监听器挂在 DeviceStateMachine 上，
 *  而 state_machine_ 是 Application 的**私有**成员（application.h:139），
 *  拿不到。走监听就得改 application.cc —— 计划里明确要求不动它（那样 S3 和
 *  stock 路径也会被牵连进来）。
 *
 *  Application::GetDeviceState() 是公开的（application.h:71），而且本回调跑在
 *  LVGL 的 lv_timer_handler 里，**已经持有 LVGL 锁**，所以在这里做 LVGL 操作
 *  是安全的，不需要 Schedule() 或额外的锁编排。
 * ========================================================================== */

/* ========================================================================== 
 *  SetupUI —— 整条链路上唯一真正的"接入点"
 * ========================================================================== */

void XiaoweiUiDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    /* 必须在 LVGL 锁内建对象：application.cc 是在 Application 任务里调
     * SetupUI()，而 lvgl_port 的 lv_timer_handler 跑在 LVGL 自己的任务上。
     * 不持锁建控件 = 和渲染线程抢 LVGL 内部链表。
     * 下面的 LcdDisplay::SetupUI() 内部会再取一次同一把锁 —— 它是
     * xSemaphoreCreateRecursiveMutex，可重入，嵌套安全。 */
    DisplayLockGuard lock(this);

    /* ---- 1) 先把聊天页的屏幕造出来，并让它成为「当前活动屏幕」 ---- */
    chat_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(chat_screen_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(chat_screen_, 0, 0);
    lv_screen_load(chat_screen_);

    /* ---- 2) 基类把「表情脸 + 状态 + 字幕 + 顶栏」建到 chat_screen_ 上 ----
     *
     * 顺序不能反：基类是用 lv_screen_active() 取屏幕的，必须先 lv_screen_load()。
     *
     * 这里也**不能**先自己调 Display::SetupUI()（上一版就是这么写的）——
     * 那会把 setup_ui_called_ 置位，而 LcdDisplay::SetupUI() 开头第一句就是
     *     if (setup_ui_called_) { warn; return; }
     * 于是基类直接空转，一个控件都不建。标记由基类自己打。 */
    LcdDisplay::SetupUI();
    chat_ui_built_ = true;

    /* ---- 3) 聊天页右滑返回 ---- */
    attach_gesture_recursive(chat_screen_);

    /* 切走时收起下拉快捷面板。面板挂在顶层，切屏带不走它。
       挂在屏幕对象自己身上而不是 display 上，原因见 quick_panel.h。 */
    xiaowei_quick_panel_watch_screen(chat_screen_);

    /* ---- 4) 登记聊天页 + 建 LCD UI 外壳并显示主屏 ----
     *
     * chat_app_set_screen() 必须在 xiaowei_ui_init() 之前：注册动作在
     * xiaowei_ui_init() 里，而 create_screen 回调返回的就是这里登记的指针。
     * （注册本身是懒的，但先设好可以避免"注册了却打不开"的中间态。）
     *
     * xiaowei_ui_init() 内部依次做
     *   app_registry_init -> register_builtin_apps -> app_manager_init
     *   -> home_screen_create -> app_list_screen_create -> home_screen_show
     * 而 home_screen_create() 用的是 lv_obj_create(NULL)（自建 screen），
     * 不是 lv_screen_active()，所以不需要预先准备任何屏幕。
     *
     * 这里从 chat_screen_ 切到主屏的这一跳也发生在同一把锁内，
     * 渲染线程看不到中间的聊天页，开机不会闪一下。 */
    chat_app_set_screen(chat_screen_);
    xiaowei_ui_init();

    /* ---- 5) 语音活动自动切到聊天页 ---- */
    /* Voice activity deliberately does not navigate away from the current page. */
    voice_watch_timer_ = nullptr;

    ESP_LOGI(TAG, "LCD UI shell is up (home screen loaded, chat screen registered)");
}

/* ==========================================================================
 *  SetTheme —— 唯一必须 override 的 setter
 *
 *  基类 LcdDisplay::SetTheme()（lcd_display.cc:1188）做两件本类不能照单全收的事：
 *
 *   1) 它把文字字体/颜色设到 lv_screen_active() 上（:1194、:1212-1213）。
 *      运行期活动屏幕通常是 LCD UI 的主屏或某个 app 页 —— 直接调等于把 xiaozhi
 *      的主题糊到 LCD UI 上。所以先临时切到 chat_screen_，做完立刻切回。
 *   2) 它**不给 current_theme_ 赋值**（全工程只有 ctor 的 lcd_display.cc:82 赋值）。
 *      这里显式接住，保证 GetTheme() 一直返回有效指针。
 *
 *  两次 lv_screen_load() 都在同一把 LVGL 锁内完成，渲染线程看不到中间态，不会闪。
 *  SetTheme 本身是低频调用（启动换主题 assets.cc:397 + MCP 运行期切主题
 *  mcp_server.cc:92），这点切换代价可以忽略。
 * ========================================================================== */

/* ==========================================================================
 *  DisplayImage —— 服务端下发的图片（AI 画图）落到聊天页上
 *
 *  图片落到 LcdDisplay::SetScreenImage()（lcd_display.cc）那块整屏控件
 *  screen_image_ 上，而它是**聊天页**（chat_screen_）的孩子 —— 建屏时跟基类
 *  那批控件一起建出来的。聊天页只是环上三页之一，开机后活动屏通常是 LCD UI
 *  主屏，所以不先切过去，图就被画在一块看不见的屏上，用户什么都不会看到。
 *
 *  切屏用 standby_ring_goto()，它的文档里就写着"给语音自动进聊天页用"
 *  （shell/standby_ring.h），语义正好：它走 set_shell_screen() 而不是 app 返回栈，
 *  不会把栈塞乱。
 *
 *  看图是**有借有还**的：切走之前先把当前那一屏记下来（screen_image_prev_screen_），
 *  图片收起后（点一下 / 停留 SCREEN_IMAGE_DURATION_MS 超时）回到它 —— 收尾逻辑见
 *  下面的 OnScreenImageHidden()。
 *
 *  加锁：本函数是从 Application 任务里 Schedule() 过来的，手上没有 LVGL 锁，
 *  而换屏是纯 LVGL 操作。锁是可重入的递归互斥量（见 SetupUI() 的说明），
 *  下面 SetScreenImage 再取一次是安全的。
 *  解码则不在这把锁里 —— 见 LvglDisplay::DisplayImage 的说明。
 * ========================================================================== */

void XiaoweiUiDisplay::DisplayImage(const uint8_t* data, size_t len) {
    lv_obj_t* previous = nullptr;
    {
        DisplayLockGuard lock(this);
        previous = lv_screen_active();
    }

    /* 先落地、再切屏（原来是反过来的）。SetScreenImage 是同步的，解码失败时它
       什么都不做，于是下面 HasScreenImage() 为假 —— 那就干脆别切屏、也别计时，
       免得解码失败还白闪一下聊天页、十秒后又自己跳回去。 */
    LvglDisplay::DisplayImage(data, len);

    {
        DisplayLockGuard lock(this);
        if (!HasScreenImage()) {
            ESP_LOGW(TAG, "DisplayImage: nothing was shown, staying on the current screen");
            return;
        }

        /* AI 画的这张也进相册 —— 三路来源（手动快门 / AI 拍照 / AI 绘画）共用
           那 10 个槽位，见 apps/camera/photo_store.h。

           必须在这里拷：整屏图的像素跟着 screen_image_cached_ 这个 unique_ptr
           走，图一收起（点击 / 超时）就被释放，出了这个作用域再想拿就是悬空的。
           photo_store 那侧自己往 PSRAM 拷一份，不持有这里的指针。

           此刻手上是显示锁 —— photo_store 的锁是叶子锁，不会反过来取显示锁，
           不成环。 */
        const lv_img_dsc_t* dsc = GetScreenImageDsc();
        if (dsc && dsc->header.cf == LV_COLOR_FORMAT_RGB565 && dsc->data) {
            xiaowei_photo_store_add(dsc->data, dsc->header.w, dsc->header.h,
                                    dsc->header.stride, XIAOWEI_PHOTO_AI_DRAW);
        }
        /* 本来就在聊天页时记下来没意义（回去也是回自己） */
        screen_image_prev_screen_ = (previous == chat_screen_) ? nullptr : previous;
        ESP_LOGI(TAG, "DisplayImage: switching to the chat page (%us on screen)",
                 (unsigned)(SCREEN_IMAGE_DURATION_MS / 1000));
        standby_ring_goto(STANDBY_MODE_CHAT);
    }

    StartScreenImageTimer();
}

/* ==========================================================================
 *  整屏图片的停留计时与"回到上一屏"
 *
 *  看图会把人从原来那一屏（可能是壁纸/表情页，也可能是天气这类 app 页）拽到聊天页，
 *  所以看完得送回去 —— 否则 AI 画完一张图，用户就被永久留在一个他不一定想待的页面上。
 *
 *  两条退出路径都汇到 LcdDisplay::HideScreenImage()：
 *      点一下图片          （lcd_display.cc 的 screen_image_clicked_cb）
 *      停留 SCREEN_IMAGE_DURATION_MS 超时（本文件的 OnScreenImageTimeout）
 *  所以"回去"只写在 OnScreenImageHidden() 一处，两条路都不会漏。
 *
 *  计时器跑在 esp_timer 任务上，**不是** LVGL 任务 —— 回调里必须自己拿 LVGL 锁，
 *  照抄 LvglDisplay ctor 里 notification_timer_ 的写法（同为 ESP_TIMER_TASK）。
 * ========================================================================== */

void XiaoweiUiDisplay::StartScreenImageTimer() {
    if (screen_image_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    static_cast<XiaoweiUiDisplay*>(arg)->OnScreenImageTimeout();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "screen_image",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &screen_image_timer_));
    }
    /* 上一张图还没到点就来第二张：把旧计时重来一遍，别让它提前把新图收走。
       esp_timer_stop 对没在跑的计时器返回 ESP_ERR_INVALID_STATE，是正常情况，不 check。 */
    esp_timer_stop(screen_image_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(screen_image_timer_, SCREEN_IMAGE_DURATION_MS * 1000));
}

void XiaoweiUiDisplay::OnScreenImageTimeout() {
    DisplayLockGuard lock(this);
    /* 图已经被点掉了（点掉时 HideScreenImage 里就停过计时器，理论上走不到这），
       保险起见还是看一眼。 */
    if (screen_image_ == nullptr || lv_obj_has_flag(screen_image_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    /* 收起图片 —— 里面会回调 OnScreenImageHidden()，由它负责切回上一屏。
       HideScreenImage 自己也会取同一把递归锁，嵌套安全。 */
    HideScreenImage();
}

void XiaoweiUiDisplay::OnScreenImageHidden() {
    if (screen_image_timer_ != nullptr) {
        esp_timer_stop(screen_image_timer_);
    }

    lv_obj_t* const previous = screen_image_prev_screen_;
    screen_image_prev_screen_ = nullptr;
    if (previous == nullptr) {
        return;  /* 看图前就在聊天页，收起即可 */
    }

    /* 这 10 秒里用户自己滑走了：图确实收起来了（它挂在聊天页上，跟着聊天页走），
       但人已经不在聊天页 —— 别把他从正在看的页面上再拽回来。 */
    if (lv_screen_active() != chat_screen_) {
        ESP_LOGI(TAG, "Screen image hidden, but the chat page is no longer active; staying put");
        return;
    }

    /* 那一屏可能已经不在了（app 页会被 destroy_screen 回收），别往悬空指针上切。 */
    if (!lv_obj_is_valid(previous)) {
        ESP_LOGW(TAG, "Previous screen is gone; staying on the chat page");
        return;
    }

    ESP_LOGI(TAG, "Screen image hidden, returning to the previous screen");
    /* 走 set_shell_screen + screen_switch 这一对，和 standby_ring 内部换页同一个写法：
       bookkeeping（s_current / 返回栈）跟着一起更新，不然 xiaowei_current_screen()
       会一直指着已经离开的聊天页。 */
    xiaowei_app_manager_set_shell_screen(previous);
    xiaowei_screen_switch(previous, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

XiaoweiUiDisplay::~XiaoweiUiDisplay() {
    if (screen_image_timer_ != nullptr) {
        esp_timer_stop(screen_image_timer_);
        esp_timer_delete(screen_image_timer_);
        screen_image_timer_ = nullptr;
    }
}

/* ==========================================================================
 *  DisplayWeather —— 服务端下发的天气数据落到天气页上
 *
 *  这是第二份"服务端主动下发"的报文（第一份是 AI 画图的 type=="display"），
 *  形状一样：application.cc 只负责把 JSON 转过来，解析和落屏都在显示层。
 *
 *  报文是给这块屏**裁剪过**的：只有英文和数字，没有图标、没有中文（本页只有
 *  Montserrat 字体）。字段定义见 ported/weather_screen.h 的 weather_data_t，
 * 服务端那份是 my-server 的 get_weather.build_device_payload()。
 *
 *  解析完调 weather_screen_apply()：屏幕还没建（用户还没点开天气）就只把数据存下来，
 *  建屏时补填 —— 服务端一般比用户手快。
 * ========================================================================== */

/* 取整数：服务端没给这个字段时是 JSON null（或字段整个缺失），
 * 一律记成 WEATHER_NO_VALUE，页面会显示 "--"。 */
static int json_int_or_na(const cJSON* obj, const char* key) {
    if (obj == nullptr) return WEATHER_NO_VALUE;
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : WEATHER_NO_VALUE;
}

/* 取字符串，顺手截断到目标缓冲长度（服务端的词最长 21 字符，缓冲 24，够用；
   截断只是为了报文异常时不要越界）。 */
static void json_text(const cJSON* obj, const char* key, char* out, size_t out_len) {
    if (out_len == 0) return;
    out[0] = '\0';
    if (obj == nullptr) return;
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return;
    std::strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
}

void XiaoweiUiDisplay::DisplayWeather(const char* json, size_t len) {
    if (json == nullptr || len == 0) {
        return;
    }

    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse weather payload (%u bytes)", (unsigned)len);
        return;
    }

    /* 1.2KB 的结构体放栈上：本函数跑在 Application 主任务（栈 10KB，见
       CONFIG_ESP_MAIN_TASK_STACK_SIZE），放得下。 */
    weather_data_t data = {};
    json_text(root, "city", data.city, sizeof(data.city));
    json_text(root, "update", data.update, sizeof(data.update));

    const cJSON* now = cJSON_GetObjectItem(root, "now");
    data.now.temp = json_int_or_na(now, "temp");
    data.now.feels = json_int_or_na(now, "feels");
    data.now.humidity = json_int_or_na(now, "humidity");
    data.now.wind = json_int_or_na(now, "wind");
    json_text(now, "cond", data.now.cond, sizeof(data.now.cond));

    const cJSON* daily = cJSON_GetObjectItem(root, "daily");
    const int daily_count = cJSON_IsArray(daily) ? cJSON_GetArraySize(daily) : 0;
    for (int i = 0; i < daily_count && data.daily_count < WEATHER_DAILY_MAX; ++i) {
        const cJSON* day = cJSON_GetArrayItem(daily, i);
        weather_daily_t* out = &data.daily[data.daily_count];
        json_text(day, "day", out->day, sizeof(out->day));
        json_text(day, "cond", out->cond, sizeof(out->cond));
        out->tmin = json_int_or_na(day, "tmin");
        out->tmax = json_int_or_na(day, "tmax");
        ++data.daily_count;
    }

    const cJSON* hourly = cJSON_GetObjectItem(root, "hourly");
    const int hourly_count = cJSON_IsArray(hourly) ? cJSON_GetArraySize(hourly) : 0;
    for (int i = 0; i < hourly_count && data.hourly_count < WEATHER_HOURLY_MAX; ++i) {
        const cJSON* hour = cJSON_GetArrayItem(hourly, i);
        weather_hourly_t* out = &data.hourly[data.hourly_count];
        json_text(hour, "time", out->time, sizeof(out->time));
        json_text(hour, "cond", out->cond, sizeof(out->cond));
        out->temp = json_int_or_na(hour, "temp");
        ++data.hourly_count;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Weather received: %s %s %dC, %d days / %d hours", data.city,
             data.now.cond, data.now.temp, data.daily_count, data.hourly_count);

    /* 落屏要动 LVGL 控件 —— 本函数是从 Application 任务 Schedule() 过来的，
       手上没有锁。天气页自己不加锁，锁在这里取（可重入，嵌套安全）。 */
    DisplayLockGuard lock(this);
    weather_screen_apply(&data);
}

void XiaoweiUiDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    current_theme_ = theme;

    /* 控件还没建就只记指针。实测这条分支现在走不到：唯一启动期调用点
     * assets.cc:397 在 Application::Initialize() 里排在 display->SetupUI() 之后。
     * 留着是为了将来有更早的调用点时不至于空指针解引用。 */
    if (!chat_ui_built_ || chat_screen_ == nullptr) {
        ESP_LOGD(TAG, "SetTheme before chat UI is built; recorded only");
        return;
    }

    lv_obj_t* const previous = lv_screen_active();
    const bool swapped = (previous != chat_screen_);
    if (swapped) lv_screen_load(chat_screen_);

    LcdDisplay::SetTheme(theme);

    if (swapped) lv_screen_load(previous);
}

void XiaoweiUiDisplay::DisplayArtifactImage(const uint8_t* data, size_t len, const char* id) {
    {
        DisplayLockGuard lock(this);
        if (qrcode_app_download_visible()) return;
        if (id && *id && !artifact_task_id_.empty() && artifact_task_id_ != id) return;
    }
    DisplayImage(data, len);
}

void XiaoweiUiDisplay::DisplayArtifactEvent(const char* json, size_t len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    auto field = [root](const char* name) -> const char* {
        auto value = cJSON_GetObjectItemCaseSensitive(root, name);
        return cJSON_IsString(value) ? value->valuestring : "";
    };
    const std::string type = field("type");
    const std::string task = field("task_id");
    const std::string state = field("state");
    DisplayLockGuard lock(this);
    auto theme = static_cast<LvglTheme*>(GetTheme());
    const lv_font_t* font = theme && theme->text_font() ? theme->text_font()->font() : LV_FONT_DEFAULT;
    if (type == "artifact_download") {
        // 停掉旧图片的自动收起计时器，再打开下载二维码
        if (screen_image_timer_) esp_timer_stop(screen_image_timer_);
        screen_image_prev_screen_ = nullptr;
        HideScreenImage();
        qrcode_app_show_download(field("url"), font);
    } else if (type == "artifact_task" && !task.empty() && task.size() <= 64) {
        const bool new_task = artifact_task_id_ != task;
        const bool accepted = artifact_task_id_.empty() || !new_task || state == "queued" || state == "running";
        if (accepted && (!qrcode_app_download_visible() || (new_task && state == "queued"))) {
            artifact_task_id_ = task;
            const bool busy = state == "queued" || state == "running";
            const char* text = field("text");
            if (state == "completed" && strcmp(field("kind"), "report") == 0)
                text = "报告已生成\n说“下载报告”获取 PDF";
            artifact_app_update(field("title"), text, busy, font);
            if (new_task && busy) {
                if (screen_image_timer_) esp_timer_stop(screen_image_timer_);
                screen_image_prev_screen_ = nullptr;
                HideScreenImage();
                xiaowei_app_open("artifact");
            }
        }
    }
    // artifact_ready 只带元数据，不能把正在显示的二维码顶掉
    cJSON_Delete(root);
}
