#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void xiaowei_screen_stack_init(void);
void xiaowei_screen_stack_push(lv_obj_t *screen);
lv_obj_t *xiaowei_screen_stack_pop(void);
lv_obj_t *xiaowei_screen_stack_top(void);
void xiaowei_screen_stack_clear(void);

#ifdef __cplusplus
}
#endif
