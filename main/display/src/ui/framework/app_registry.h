#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *title;
    const lv_image_dsc_t *icon;
    lv_obj_t *(*create_screen)(void);
    void (*on_open)(void);
    void (*on_close)(void);
    void (*destroy_screen)(void);
} xiaowei_app_t;

void xiaowei_app_registry_init(void);
bool xiaowei_app_register(const xiaowei_app_t *app);
const xiaowei_app_t *xiaowei_app_find(const char *id);
size_t xiaowei_app_count(void);
const xiaowei_app_t *xiaowei_app_at(size_t index);

#ifdef __cplusplus
}
#endif
