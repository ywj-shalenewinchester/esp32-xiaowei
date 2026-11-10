#pragma once
#include "../../framework/app_registry.h"
#ifdef __cplusplus
extern "C" {
#endif
const xiaowei_app_t *artifact_app_descriptor(void);
void artifact_app_update(const char *title, const char *text, bool busy, const lv_font_t *font);
#ifdef __cplusplus
}
#endif
