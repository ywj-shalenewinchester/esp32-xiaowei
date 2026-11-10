#include "menu_layout.h"

void menu_layout_apply(lv_obj_t *list) {
    lv_obj_set_width(list, 430);
    lv_obj_set_height(list, 410);
    lv_obj_center(list);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 5, LV_PART_MAIN);
}
