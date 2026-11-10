#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕正下方常驻的帧率标签：`CAM xx  UI xx`。
   CAM = 摄像头实际出帧率（xiaowei_camera_get_fps()），UI = LVGL 真实重绘率。

   挂在 lv_layer_top() 上，不属于任何 screen，所以切页（app_manager /
   standby_ring / lv_screen_load_anim）都不会把它带走 —— 全局悬浮。

   只建一次，重复调用直接返回。调用方需已持有 LVGL 锁。 */
void xiaowei_fps_overlay_init(void);

#ifdef __cplusplus
}
#endif
