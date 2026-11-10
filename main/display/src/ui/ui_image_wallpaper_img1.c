#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)
#include <lvgl.h>
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_WALLPAPER_IMG1
#define LV_ATTRIBUTE_IMG_WALLPAPER_IMG1
#endif

/* 像素数据已外置到 uiimg 分区（由 scripts/build_ui_images_bin.py 打成 mmap 容器），
 * 运行期由 xiaowei_ui_bind_external_assets() 填进上面的 .data。
 * 别把数据搬回这个文件：它进的是 app 槽（5 MB），而 app 槽已经放不下了。 */


lv_image_dsc_t img_wallpaper_img1 = {
  .header = {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_RGB565,
    .flags = 0,
    .w = 466,
    .h = 466,
    .stride = 932,
    .reserved_2 = 0,
  },
  .data_size = 434312,
  .data = NULL,
  .reserved = NULL,
};