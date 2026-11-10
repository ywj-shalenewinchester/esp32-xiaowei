#pragma once

#include "app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

void xiaowei_app_manager_init(void);
/* Tell the manager which shell screen is currently visible.  This clears any
 * stale application stack when home/app-list switch directly without opening
 * an app first. */
void xiaowei_app_manager_set_shell_screen(lv_obj_t *screen);
bool xiaowei_app_open(const char *id);
void xiaowei_app_back(void);
void xiaowei_app_close(void);
const xiaowei_app_t *xiaowei_app_current(void);
lv_obj_t *xiaowei_current_screen(void);

/* 切页（唯一的换屏入口）。动效时长在 app_manager.c 的 SCREEN_LOAD_ANIM_MS 里，
   0 表示不做位移动画。**所有**换屏都走这里，别自己调 lv_screen_load_anim ——
   共用一个旋钮，否则调完一处、另一处照旧抖（历史上 app_list_screen.c /
   home_screen.c 就各自写死过 220ms）。

   anim 按手势方向传（几何见 display/lv_display.c:848-883）：
     MOVE_LEFT  = 新页从屏幕右边进 —— 左滑前进
     MOVE_RIGHT = 新页从屏幕左边进 —— 右滑返回

   本函数还会在换屏后作废当前这一"按"（手指没抬就一直不收输入），
   否则同一次拖拽会反复触发手势、连翻好几页。 */
void xiaowei_screen_switch(lv_obj_t *screen, lv_screen_load_anim_t anim);

#ifdef __cplusplus
}
#endif
