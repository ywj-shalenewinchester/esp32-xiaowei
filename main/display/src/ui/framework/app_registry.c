#include "app_registry.h"

#include <string.h>

#define XIAOWEI_MAX_APPS 32

static const xiaowei_app_t *s_apps[XIAOWEI_MAX_APPS];
static size_t s_app_count;

void xiaowei_app_registry_init(void) {
    memset(s_apps, 0, sizeof(s_apps));
    s_app_count = 0;
}

bool xiaowei_app_register(const xiaowei_app_t *app) {
    if (!app || !app->id || !app->create_screen || s_app_count >= XIAOWEI_MAX_APPS) return false;
    if (xiaowei_app_find(app->id)) return false;
    s_apps[s_app_count++] = app;
    return true;
}

const xiaowei_app_t *xiaowei_app_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < s_app_count; ++i)
        if (strcmp(s_apps[i]->id, id) == 0) return s_apps[i];
    return NULL;
}

size_t xiaowei_app_count(void) { return s_app_count; }

const xiaowei_app_t *xiaowei_app_at(size_t index) {
    return index < s_app_count ? s_apps[index] : NULL;
}
