#pragma once

#include "../../framework/app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

const xiaowei_app_t *qrcode_app_descriptor(void);
bool qrcode_app_show_download(const char *url, const lv_font_t *font);
bool qrcode_app_download_visible(void);

#ifdef __cplusplus
}
#endif
