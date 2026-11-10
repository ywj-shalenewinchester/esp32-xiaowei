#pragma once
#include "lvgl/lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
lv_obj_t *home_screen_create(void);
void home_screen_show(void);
void home_screen_next_wallpaper(void);

/* 只读取值：不建屏，屏还没建过时返回 NULL。
 * 给 standby_ring 判断"当前活动屏是不是壁纸页"用 —— 那种场合不能顺手把屏建出来。 */
lv_obj_t *home_screen_get(void);
#ifdef __cplusplus
}
#endif
