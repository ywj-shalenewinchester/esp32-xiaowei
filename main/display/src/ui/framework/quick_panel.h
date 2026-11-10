#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 下拉快捷面板（音量 / 亮度 / 二维码三个按钮）。

   原来它是 home_screen.c 里 s_home 的私有子对象，所以只有壁纸页能拉出来。
   现在挂在 lv_layer_top() 上，和 fps_overlay 一样不属于任何 screen ——
   待机环上的壁纸 / 聊天 / 表情三页共用同一个实例。

   为什么不用"每页各建一个"：面板的状态（开着/关着）必须只有一个真相，
   而且切页时它得能自己收起（见下），多实例反而要处理三份状态。

   生命周期约定：
     - init()         开机建一次，幂等。调用方需已持有 LVGL 锁。
     - watch_screen() 每块**待机**屏建屏时调一次，负责"切走时自动收起"。
     - open()/close() 开合。close() 在面板还没建时是安全的空操作。 */

void xiaowei_quick_panel_init(void);

/* 让面板在这块屏被卸载时自动收起。
 *
 * 必须挂在屏幕自己身上，不能像 fps_overlay 那样挂 display 的
 * LV_EVENT_SCREEN_UNLOAD_START：引擎只在**立即切换**路径发屏幕级事件
 * （core/lv_display.c:1445-1474），而本工程五处切屏全是 220ms 动画路径，
 * 那个钩子会静默失效。挂在屏幕对象上则在两条路径都会发。
 *
 * 同一个屏幕重复登记只会挂一次。 */
void xiaowei_quick_panel_watch_screen(lv_obj_t *screen);

void xiaowei_quick_panel_open(void);
void xiaowei_quick_panel_close(void);

#ifdef __cplusplus
}
#endif
