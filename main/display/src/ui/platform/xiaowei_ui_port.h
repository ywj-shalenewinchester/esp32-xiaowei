#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int xiaowei_ui_get_brightness(void);
bool xiaowei_ui_set_brightness(int value);
int xiaowei_ui_get_volume(void);
bool xiaowei_ui_set_volume(int value);
void xiaowei_ui_show_notification(const char *title, const char *message);
void xiaowei_ui_get_date(int *year, int *month, int *day);
void xiaowei_ui_get_time(int *hour, int *minute);
void xiaowei_ui_on_enter_app(const char *id);
void xiaowei_ui_on_exit_app(const char *id);

#ifdef __cplusplus
}
#endif
