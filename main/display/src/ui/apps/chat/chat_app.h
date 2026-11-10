#pragma once

#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

const xiaowei_app_t *chat_app_descriptor(void);

/*
 * 聊天页的屏幕**不是在这里建的**，而是由 main/display/xiaowei_ui_display.cc 的
 * XiaoweiUiDisplay::SetupUI() 建好后登记进来。
 *
 * 为什么要这么绕：这一页要的是 xiaozhi 原来那套「表情脸 + 状态 + 字幕」，而它整套
 * 控件的创建逻辑都在 LcdDisplay::SetupUI() 里。那个函数用 lv_screen_active() 取屏幕，
 * 所以只要显示类先把本页的屏幕设成活动屏幕、再调基类 SetupUI()，基类就会把控件
 * 建到这块屏上 —— 于是 SetEmotion/SetStatus/SetChatMessage/ClearChatMessages
 * 全都可以**直接复用基类实现**，一行渲染代码都不用重写。
 *
 * 代价是屏幕的所有权在显示类手里（见下面 destroy_screen = NULL 的说明）。
 *
 * 必须在 xiaowei_ui_init() 之前调用一次，否则 create_screen 返回 NULL，
 * xiaowei_app_open("chat") 会静默失败（左滑看起来「没反应」）。
 */
void chat_app_set_screen(lv_obj_t *screen);

/* registry 的 create_screen 回调：返回上面登记的那块屏。 */
lv_obj_t *chat_app_screen(void);

#ifdef __cplusplus
}
#endif
