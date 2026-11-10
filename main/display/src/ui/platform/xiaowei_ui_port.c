#include "xiaowei_ui_port.h"

#include <time.h>

extern int xiaowei_get_brightness(void);
extern bool xiaowei_set_brightness(int value);

__attribute__((weak)) int xiaowei_audio_get_volume(void) { return 50; }
__attribute__((weak)) bool xiaowei_audio_set_volume(int value) { (void)value; return false; }
__attribute__((weak)) void xiaowei_ui_show_notification(const char *title, const char *message) {
    (void)title; (void)message;
}

int xiaowei_ui_get_brightness(void) { return xiaowei_get_brightness(); }
bool xiaowei_ui_set_brightness(int value) { return xiaowei_set_brightness(value); }
int xiaowei_ui_get_volume(void) { return xiaowei_audio_get_volume(); }
bool xiaowei_ui_set_volume(int value) { return xiaowei_audio_set_volume(value); }

void xiaowei_ui_get_date(int *year, int *month, int *day) {
    time_t now = time(NULL);
    struct tm local = {0};
    localtime_r(&now, &local);
    if (year) *year = local.tm_year + 1900;
    if (month) *month = local.tm_mon + 1;
    if (day) *day = local.tm_mday;
}

void xiaowei_ui_get_time(int *hour, int *minute) {
    time_t now = time(NULL);
    struct tm local = {0};
    localtime_r(&now, &local);
    if (hour) *hour = local.tm_hour;
    if (minute) *minute = local.tm_min;
}

void xiaowei_ui_on_enter_app(const char *id) { (void)id; }
void xiaowei_ui_on_exit_app(const char *id) { (void)id; }
