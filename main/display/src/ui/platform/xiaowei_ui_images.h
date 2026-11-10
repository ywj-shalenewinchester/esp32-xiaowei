/*
 * xiaowei_ui_images.h
 *
 * 把外置到 uiimg 分区的 UI 大图绑回各自的 lv_image_dsc_t。
 *
 * 背景：3 张 466x466 壁纸的像素数据原本内嵌在 .c 里（外加 5 张姿态仪图，
 * 随 IMU 测试页重做已经删掉），共 2.2 MB。
 * 接入 LCD UI 后 P4 固件到了 6.74 MB，双 OTA 布局下 app 槽只有 5 MB，装不下；
 * 更要命的是 robot 页要用的 emote 表情包（1.73 MB）彻底没地方放。
 * 于是这些大图的像素数据被抽到 uiimg 分区（scripts/externalize_ui_images.py），
 * 源码里只留下描述符（尺寸/颜色格式要编译期可见），.data = NULL，
 * 由本模块在开机时填上。
 *
 * 调用时机：**必须在任何一处 lv_image_set_src() 之前**。实际挂在
 * xiaowei_ui_init() 的最开头，早于 home_screen_create() 里那次壁纸 set_src。
 * 绑晚了不光是"没图"—— LVGL 会拿 NULL 当像素基址去读。
 *
 * 失败时的行为：逐张打 ERROR 日志，并把失败的描述符指向一块 PSRAM 里
 * 零填充的缓冲（黑图），而不是留 NULL。宁可黑屏也不要崩。
 */
#ifndef XIAOWEI_UI_IMAGES_H
#define XIAOWEI_UI_IMAGES_H

#ifdef __cplusplus
extern "C" {
#endif

/* 幂等：重复调用只做一次，第二次直接返回。 */
void xiaowei_ui_bind_external_assets(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOWEI_UI_IMAGES_H */
