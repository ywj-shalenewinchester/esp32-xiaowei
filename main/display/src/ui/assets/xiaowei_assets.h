#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 三张壁纸的像素数据已外置到 uiimg 分区，描述符必须**非 const**：
 * 它的 .data 由 xiaowei_ui_bind_external_assets() 在开机时填进来
 * （实现在 main/display/src/ui/platform/xiaowei_ui_images.c，
 *   由 xiaowei_ui_init() 在 home_screen_create() 之前调用）。
 * 本文件里其余的图仍然是内嵌字节数组，保持 const 不变。 */
extern lv_image_dsc_t img_wallpaper_img1;
extern lv_image_dsc_t img_wallpaper_img2;
extern lv_image_dsc_t img_wallpaper_img3;
extern const lv_image_dsc_t img_flashlight;
extern const lv_image_dsc_t img_timer;
extern const lv_image_dsc_t img_calendar;
extern const lv_image_dsc_t img_ai_face;
extern const lv_image_dsc_t img_camera_img;
extern const lv_image_dsc_t img_weather_img;
extern const lv_image_dsc_t img_music_img;
extern const lv_image_dsc_t img_game_img;
extern const lv_image_dsc_t img_tools_img;
extern const lv_image_dsc_t img_hardware_img;
extern const lv_image_dsc_t img_settings_img;
extern const lv_image_dsc_t img_robot_outline_img;
extern const lv_image_dsc_t img_aivoice;
extern const lv_image_dsc_t img_ai_draw;
extern const lv_image_dsc_t img_take_photo;
extern const lv_image_dsc_t img_photo;
extern const lv_image_dsc_t img_appearance;
extern const lv_image_dsc_t img_interaction;
extern const lv_image_dsc_t img_display;
extern const lv_image_dsc_t img_sound;
extern const lv_image_dsc_t img_haptic;
extern const lv_image_dsc_t img_network;
extern const lv_image_dsc_t img_language;
extern const lv_image_dsc_t img_system;
extern const lv_image_dsc_t img_about;
extern const lv_image_dsc_t img_alarm;
extern const lv_image_dsc_t img_audio_img;
extern const lv_image_dsc_t img_imu_img;
extern const lv_image_dsc_t img_motor_img;

#define xiaowei_asset_wallpaper_1 img_wallpaper_img1
#define xiaowei_asset_wallpaper_2 img_wallpaper_img2
#define xiaowei_asset_wallpaper_3 img_wallpaper_img3
#define xiaowei_asset_icon_flashlight img_flashlight
#define xiaowei_asset_icon_timer img_timer
#define xiaowei_asset_icon_calendar img_calendar
#define xiaowei_asset_icon_ai img_ai_face
#define xiaowei_asset_icon_camera img_camera_img
#define xiaowei_asset_icon_weather img_weather_img
#define xiaowei_asset_icon_music img_music_img
#define xiaowei_asset_icon_game img_game_img
#define xiaowei_asset_icon_tools img_tools_img
#define xiaowei_asset_icon_test img_hardware_img
#define xiaowei_asset_icon_settings img_settings_img
#define xiaowei_asset_icon_robot_outline img_robot_outline_img
#define xiaowei_asset_icon_robot img_ai_face
#define xiaowei_asset_icon_ai_voice img_aivoice
#define xiaowei_asset_icon_ai_draw img_ai_draw
#define xiaowei_asset_icon_take_photo img_take_photo
#define xiaowei_asset_icon_photo img_photo
#define xiaowei_asset_icon_appearance img_appearance
#define xiaowei_asset_icon_interaction img_interaction
#define xiaowei_asset_icon_display img_display
#define xiaowei_asset_icon_sound img_sound
#define xiaowei_asset_icon_haptic img_haptic
#define xiaowei_asset_icon_network img_network
#define xiaowei_asset_icon_language img_language
#define xiaowei_asset_icon_system img_system
#define xiaowei_asset_icon_about img_about
#define xiaowei_asset_icon_alarm img_alarm
#define xiaowei_asset_icon_audio img_audio_img
#define xiaowei_asset_icon_imu img_imu_img
#define xiaowei_asset_icon_motor img_motor_img

#ifdef __cplusplus
}
#endif
