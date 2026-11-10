#include "xiaowei_ui.h"
#include "framework/app_registry.h"
#include "framework/app_manager.h"
#include "framework/fps_overlay.h"
#include "framework/quick_panel.h"
#include "shell/home_screen.h"
#include "shell/app_list_screen.h"
#include "apps/calendar/calendar_app.h"
#include "apps/timer/timer_app.h"
#include "apps/flashlight/flashlight_app.h"
#include "apps/legacy/legacy_app.h"
#include "apps/camera/camera_app.h"
#include "apps/audio/audio_app.h"
#include "apps/robot/robot_app.h"
#include "apps/imu/imu_app.h"
#include "apps/qrcode/qrcode_app.h"
#include "apps/artifact/artifact_app.h"
#include "apps/chat/chat_app.h"
#include "platform/xiaowei_ui_images.h"

void xiaowei_register_builtin_apps(void) {
    /* 旧工程的八个主菜单入口，顺序保持不变。 */
    xiaowei_app_register(legacy_ai_descriptor());
    xiaowei_app_register(legacy_camera_descriptor());
    xiaowei_app_register(legacy_weather_descriptor());
    xiaowei_app_register(legacy_music_descriptor());
    xiaowei_app_register(legacy_game_descriptor());
    xiaowei_app_register(legacy_tools_descriptor());
    xiaowei_app_register(legacy_test_descriptor());
    xiaowei_app_register(legacy_settings_descriptor());
    /* 工具页使用统一应用对象打开这三个应用。 */
    xiaowei_app_register(flashlight_app_descriptor());
    xiaowei_app_register(timer_app_descriptor());
    xiaowei_app_register(calendar_app_descriptor());
    xiaowei_app_register(camera_live_app_descriptor());
    xiaowei_app_register(camera_capture_app_descriptor());
    xiaowei_app_register(camera_album_app_descriptor());
    xiaowei_app_register(audio_test_app_descriptor());
    xiaowei_app_register(robot_app_descriptor());
    /* 测试页的 IMU 项（见 legacy_app.c 的 s_test_items）按 id 打开它。 */
    xiaowei_app_register(imu_app_descriptor());
    /* Secondary pages are registered applications as well.  Their create
     * callbacks use the pending selection set by legacy_app.c, so all
     * navigation still goes through the same manager and screen stack. */
    xiaowei_app_register(legacy_info_descriptor());
    xiaowei_app_register(legacy_motor_descriptor());
    xiaowei_app_register(legacy_brightness_descriptor());
    xiaowei_app_register(legacy_volume_descriptor());
    xiaowei_app_register(legacy_haptic_descriptor());
    xiaowei_app_register(legacy_settings_group_descriptor());
    xiaowei_app_register(qrcode_app_descriptor());
    xiaowei_app_register(artifact_app_descriptor());
    /* 新增项（不在旧工程的八个入口里）：xiaozhi 的聊天界面接入成一个 app，
       主屏左滑直接进（见 shell/home_screen.c 的 LV_DIR_LEFT 分支）。 */
    xiaowei_app_register(chat_app_descriptor());
}

void xiaowei_ui_init(void) {
    /* 第一件事，必须在任何 lv_image_set_src() 之前：
     * 3 张全屏壁纸的像素数据在 uiimg 分区里，描述符的 .data 还是 NULL。
     * home_screen_create() 下面就要用壁纸。
     * 详见 platform/xiaowei_ui_images.c。 */
    xiaowei_ui_bind_external_assets();

    xiaowei_app_registry_init();
    xiaowei_register_builtin_apps();
    xiaowei_app_manager_init();
    home_screen_create();
    app_list_screen_create();
    home_screen_show();

    /* 快捷面板必须排在帧率标签**前面**：顶层同层的兄弟是后建的画在上面
       （core/lv_refr.c:1116-1129），面板是不透明的全屏对象，晚建就会把
       帧率标签整个盖住。

       面板用的两个图标（xiaowei_asset_icon_sound / _display）的像素数据要等
       最上面那句 xiaowei_ui_bind_external_assets() 绑完才有 —— 所以不能提到
       它前面去；排在最后则只是为了让"顶层现在有哪几样东西"一眼可读。

       本函数由 XiaoweiUiDisplay::SetupUI() 调用，而 SetupUI 全程持有
       DisplayLockGuard，所以这里建控件已经在 LVGL 锁内，不用再加锁。 */
    xiaowei_quick_panel_init();

    /* 最后建：帧率标签挂在顶层，不参与上面这些页面的创建。放在这里是为了让
       它拿到的是已经就绪的 display（此时第一帧还没画，标签会被第一次重绘带出去）。 */
    xiaowei_fps_overlay_init();
}
