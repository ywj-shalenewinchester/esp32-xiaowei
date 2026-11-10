#pragma once

#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

const xiaowei_app_t *legacy_ai_descriptor(void);
const xiaowei_app_t *legacy_camera_descriptor(void);
const xiaowei_app_t *legacy_weather_descriptor(void);
const xiaowei_app_t *legacy_music_descriptor(void);
const xiaowei_app_t *legacy_game_descriptor(void);
const xiaowei_app_t *legacy_tools_descriptor(void);
const xiaowei_app_t *legacy_test_descriptor(void);
const xiaowei_app_t *legacy_settings_descriptor(void);
const xiaowei_app_t *legacy_info_descriptor(void);
const xiaowei_app_t *legacy_motor_descriptor(void);
const xiaowei_app_t *legacy_brightness_descriptor(void);
const xiaowei_app_t *legacy_volume_descriptor(void);
const xiaowei_app_t *legacy_haptic_descriptor(void);
const xiaowei_app_t *legacy_settings_group_descriptor(void);

#ifdef __cplusplus
}
#endif
