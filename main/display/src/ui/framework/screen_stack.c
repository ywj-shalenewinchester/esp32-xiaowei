#include "screen_stack.h"

#define XIAOWEI_SCREEN_STACK_MAX 8
static lv_obj_t *s_stack[XIAOWEI_SCREEN_STACK_MAX];
static size_t s_depth;

void xiaowei_screen_stack_init(void) { s_depth = 0; }
void xiaowei_screen_stack_push(lv_obj_t *screen) {
    if (screen && s_depth < XIAOWEI_SCREEN_STACK_MAX) s_stack[s_depth++] = screen;
}
lv_obj_t *xiaowei_screen_stack_pop(void) {
    return s_depth ? s_stack[--s_depth] : NULL;
}
lv_obj_t *xiaowei_screen_stack_top(void) {
    return s_depth ? s_stack[s_depth - 1] : NULL;
}
void xiaowei_screen_stack_clear(void) { s_depth = 0; }
