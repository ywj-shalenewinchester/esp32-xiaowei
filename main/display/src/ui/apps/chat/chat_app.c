#include "chat_app.h"
#include "../../assets/xiaowei_assets.h"

/* 由 XiaoweiUiDisplay::SetupUI() 登记，见 chat_app.h 的说明。 */
static lv_obj_t *s_screen;

void chat_app_set_screen(lv_obj_t *screen) { s_screen = screen; }
lv_obj_t *chat_app_screen(void) { return s_screen; }

static const xiaowei_app_t s_descriptor = {
    .id = "chat",
    .title = "语音助手",
    /* 复用「AI助手」那个图标，不新增资源。 */
    .icon = &xiaowei_asset_icon_ai,
    .create_screen = chat_app_screen,
    /* 每次打开都是同一块屏，没有 per-open 的初始化要做：
       表情/状态/字幕由 xiaozhi 的 Application 在状态变化时直接推给显示类。 */
    .on_open = NULL,
    .on_close = NULL,
    /* ⚠ 千万不能设销毁函数。
       这块屏上挂的是**基类的成员控件**（emoji_box_ / status_label_ /
       chat_message_label_ …），基类的 setter 一直持有这些裸指针。
       把屏销毁掉，下一句 SetEmotion() 就是 use-after-free；
       而且基类 SetupUI() 有 setup_ui_called_ 防重入，销毁后也重建不回来。 */
    .destroy_screen = NULL
};

const xiaowei_app_t *chat_app_descriptor(void) { return &s_descriptor; }
