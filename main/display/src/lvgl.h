#pragma once

/*
 * 转发壳 —— 给 src/ui/ 下那 33 个 ui_image_*.c 用的。
 *
 * 那些文件（LVGL 图片资源，从 lcd 工程整体拷过来的）第 14 行都写的是
 *     #include "../lvgl.h"
 * 而它们在 src/ui/ 里，所以 "../lvgl.h" 展开就是**本文件**。
 *
 * donor 那边为什么能编过：
 *   它把 managed_components/lvgl__lvgl/include 手动塞进了 INCLUDE_DIRS。
 *   于是引号包含按包含路径逐个回退时，命中
 *       managed_components/lvgl__lvgl/include/../lvgl.h
 *       == managed_components/lvgl__lvgl/lvgl.h   ← LVGL 组件根目录的那份真身。
 *   纯属巧合，但确实能编。
 *
 * 本工程不把那个目录放进包含路径（理由见 ui/lvgl/lvgl.h 里的说明：会让 30 多个
 * 组件的 config.h 互相遮蔽）。所以在这里补一个同名转发文件，把那 33 个文件
 * 一行都不用改地接上：
 *
 *     src/ui/ui_image_about.c --"../lvgl.h"--> src/lvgl.h --<lvgl.h>--> 真身
 *
 * 注意与 ui/lvgl/lvgl.h 是**两个不同位置**的壳，各自服务一种写法，都不要删：
 *   src/ui/lvgl/lvgl.h  <-  #include "lvgl/lvgl.h"   （49 个文件）
 *   src/lvgl.h          <-  #include "../lvgl.h"     （33 个图片资源文件）
 */
#include <lvgl.h>
