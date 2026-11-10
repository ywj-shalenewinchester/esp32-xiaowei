#pragma once
#include "lvgl/lvgl.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 二级菜单的统一形式 —— **就是「测试」「工具」「AI助手」那几页的样子**：
 * 顶部一个中文标题，下面一张 lv_list，每条 = 左图标 + 右文字（304x90）。
 *
 * 相机、游戏都走这一份。之前三个二级菜单三种长相（相机两个大圆图标没有字、
 * 音乐是中文 lv_list、游戏是一个没图标的圆角条），根子就是每家自己建控件；
 * 现在连「测试/工具/AI助手」自己也是调这里，只有一份实现，改一处三处一起变。
 *
 * 文字用的是 ui_font_font_50_menu_cn（见 fonts.h）—— 和「测试」页里的
 * "IMU"/"audio"/"motor" 同一支字体，所以英文条目也是这个字号，不需要另配。
 */
typedef struct {
    const lv_image_dsc_t *icon;   /* 允许 NULL —— 这就成了纯文字条目 */
    const char *label;            /* 条目文字。不拷贝，调用方保证常驻 */
    lv_event_cb_t on_event;       /* 挂在条目按钮上（LV_EVENT_ALL）；NULL = 不可点 */
    void *user_data;              /* 原样转给 on_event；NULL 也行 */
} xiaowei_submenu_item_t;

/* 把「标题 + 条目列表」铺进 screen。
 *
 * screen 由调用方建、调用方持有（这些二级页都是常驻复用、不销毁的），
 * 本函数只往上放控件，不碰屏幕的背景和手势 —— 右滑返回仍由各自的
 * 事件回调负责。几何、字号、配色全部定死在 submenu.c 里。
 */
void xiaowei_submenu_build(lv_obj_t *screen, const char *title,
                           const xiaowei_submenu_item_t *items, size_t count);

#ifdef __cplusplus
}
#endif
