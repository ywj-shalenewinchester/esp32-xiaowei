#pragma once

#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

const xiaowei_app_t *camera_live_app_descriptor(void);
const xiaowei_app_t *camera_capture_app_descriptor(void);
const xiaowei_app_t *camera_album_app_descriptor(void);
lv_obj_t *camera_menu_create_for_app(void);

#ifdef __cplusplus
}
#endif
