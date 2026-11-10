#include "submenu.h"

#include "../fonts.h"

/* ---------------- 二级页列表的统一规格 ----------------
 *
 * 这组数字原本散在 legacy_app.c 的 build_list() 里，是旧工程二级页沿用的
 * **圆屏安全区**：屏幕是 466x466 的圆，列表不能顶满，否则四角的条目被圆边切掉。
 * 现在提到这里当唯一一份，测试/工具/AI助手、相机、游戏都按它排。
 */
#define LIST_X        73
#define LIST_Y        100
#define LIST_W        320
#define LIST_H        350
#define LIST_PAD      8
#define LIST_ROW_GAP  6
#define ITEM_W        304
#define ITEM_H        90
#define ITEM_PAD_LEFT 16

/* 标题条：比条目小一号。原来标题和条目同用 50 号，圆屏上一个两字标题顶掉
 * 一大块，所以标题降到 30 号（ui_font_font_30_menu_cn）。
 *
 * 字号不是随便挑的：本工程这几个中文 UI 字体各自只烧进了一小撮字形，
 * 换字体先得看字在不在里面 —— 只有 50_menu_cn（208 字）和 30_menu_cn（109 字）
 * 收了"相机/游戏/工具/设置/音色"这批菜单字，40_cn/32_cn/28_cn 一个都没有。
 * 30_menu_cn 收全了这些，外加整套 ASCII；唯独没有"测/试"，所以那一页的标题
 * 写成 "TEST"（英文照排，同一个字号）。 */
#define TITLE_FONT    (&ui_font_font_30_menu_cn)
#define TITLE_Y       24

#define TEXT_COLOR    0xF9F5F5

void xiaowei_submenu_build(lv_obj_t *screen, const char *title,
                           const xiaowei_submenu_item_t *items, size_t count)
{
    if (!screen) return;

    if (title && title[0]) {
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, TITLE_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TEXT_COLOR), 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    }

    if (!items || count == 0) return;

    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_set_pos(list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, LIST_PAD, 0);
    lv_obj_set_style_pad_row(list, LIST_ROW_GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *button = lv_list_add_button(list, items[i].icon, items[i].label);
        lv_obj_set_width(button, ITEM_W);
        lv_obj_set_height(button, ITEM_H);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        /* 上下内边距必须清成 0。
         *
         * lv_list_add_button() 建的按钮是 LV_FLEX_FLOW_ROW 容器，图标和文字是它的
         * flex 子项，而 flex 是**按内容区**排的（lv_flex.c:254 用的就是
         * lv_obj_get_content_height()），内容区要扣掉 padding；可绘制又被裁在按钮
         * 的整个外框上。于是主题给按钮留下的那点 pad_top，会把 90x90 的图标整个
         * 往下推 pad_top 像素 —— 图标正好和行高一样高（素材就是 90x90），多出来的
         * 那几像素落在框外，被裁掉：看上去就是"图标显示不全"。
         * 只写 pad_left 是不够的，得先把四个方向清掉再单独补左边距。 */
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_pad_left(button, ITEM_PAD_LEFT, 0);
        if (items[i].on_event)
            lv_obj_add_event_cb(button, items[i].on_event, LV_EVENT_ALL,
                                items[i].user_data);
        /* lv_list_add_button() 的返回值是按钮，[0] 是图标（没图标时就是文字），
           [1] 才是文字 —— 取 [1] 统一按文字处理，缺了就当没有（图标也没有）。 */
        lv_obj_t *label = lv_obj_get_child(button, 1);
        if (label) {
            lv_obj_set_style_text_font(label, &ui_font_font_50_menu_cn, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(TEXT_COLOR), 0);
            lv_obj_set_width(label, LV_SIZE_CONTENT);
        }
    }
}
