#pragma once

/*
 * 转发 shim：把 UI 里的 #include "lvgl/lvgl.h" 引到真身。
 *
 * ui/ 这套代码原来在 lcd 工程里，那边用的是官方 LVGL 源码树，lvgl.h 位于
 * 仓库根的 lvgl/ 子目录下，所以到处写 "lvgl/lvgl.h"。
 *
 * 本工程用的是 managed component（managed_components/lvgl__lvgl/），它的公开
 * include 目录就是组件根目录，所以本工程自己的代码一律写 <lvgl.h>；
 * "lvgl/lvgl.h" 则找不到。
 *
 * 与其改 49 个 UI 源文件，不如在 ui/ 下补一个同名相对路径把它转发过去。
 * 因为 display/src/ui 已经在 INCLUDE_DIRS 里，这个文件就会被命中。
 *
 * 为什么不用「把 managed_components/ 加进 include path」这个更省事的办法：
 * 那会把 30 多个组件的根目录全部暴露出来，config.h / lvgl.h 这类同名头文件
 * 会互相遮蔽（例如 haptic_hv_port.h 就 #include "config.h"，会撞上别的组件的
 * config.h），风险远大于多一个转发文件。
 */
#include <lvgl.h>
