#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

const xiaowei_app_t *robot_app_descriptor(void);

/* 只读取值：不建屏，屏还没建过时返回 NULL。
 * 给 standby_ring 判断"当前活动屏是不是表情页"用 —— 那种场合不能顺手把屏建出来，
 * 否则一进主菜单就把 434 KB 帧缓冲和 emote 播放器拉起来了。 */
lv_obj_t *robot_app_screen(void);

/* 建/取表情页（幂等，已建过直接返回）。给 standby_ring 切到表情模式时用。 */
lv_obj_t *robot_app_create_screen(void);

/* 待机上下文开关。这个页面有**两种身份**，手势完全不同，详见 robot_app.c 里的长注释：
 *
 *   true  —— 主页的表情模式（"锁屏壁纸"）：右滑下一个模式、左滑主菜单，
 *            只显示用户放置好的那一张，**没有**编辑层。
 *   false —— 测试→Robot（"壁纸商店"）：左滑换表情、右滑返回（**和原来完全一致**），
 *            单击出编辑层，层里一个按钮把当前这张放置到主页。
 *
 * 幂等：重复置同值安全，播放器判空保护，未建屏时也只记状态。 */
void robot_app_set_standby(bool on);

#ifdef __cplusplus
}
#endif
