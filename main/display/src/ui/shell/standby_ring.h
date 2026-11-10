#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  待机屏三模式轮播（壁纸 / 聊天 / 表情）
 *
 *  这三页互为**同级**的待机状态，靠**左滑**循环切换：
 *
 *      壁纸 --左滑--> 聊天 --左滑--> 表情 --左滑--> 壁纸（循环）
 *
 *  三页上**右滑一律进应用主菜单**，而不是"返回上一页"。
 *  （方向与主页改动前同向：原来就是右滑进菜单、左滑往后走。）
 *
 *  ⚠ 「当前在第几页」不存状态，由 lv_screen_active() 反推
 *  （见 standby_ring_current 的实现）。因为用户可以右滑从应用菜单直接回壁纸页
 *  （app_list_screen.c 的 LV_DIR_RIGHT 分支），那条路绕过了本模块；存一份
 *  "当前索引"就一定会和真实活动屏错位。
 *
 *  ⚠ 本模块**不**用 app 返回栈。环上三页没有"返回"语义，栈必须一直是空的，
 *  详见 standby_ring.c 里 standby_ring_goto() 的注释。
 * ========================================================================== */

typedef enum {
    STANDBY_MODE_WALLPAPER = 0, /* 壁纸：日期 + 时间 + 壁纸 */
    STANDBY_MODE_CHAT      = 1, /* 聊天：xiaozhi 原来的表情脸/状态/字幕界面 */
    STANDBY_MODE_EMOTE     = 2, /* 表情：只显示"放置"好的那张表情动画，不出编辑层 */
    STANDBY_MODE_COUNT         /* 不是模式，是"当前屏不在环上"的哨兵值 */
} standby_mode_t;

/* 左滑：环上下一个模式。当前屏不在环上时不做任何事（不越权接管其它 app 页）。 */
void standby_ring_next(void);

/* 右滑：进应用主菜单。三页统一走这里。 */
void standby_ring_show_menu(void);
/* Return from the application menu to the standby mode active before it was opened. */
void standby_ring_return_from_menu(void);

/* 直接跳到某个模式。给语音自动进聊天页用。 */
void standby_ring_goto(standby_mode_t mode);

/* 当前活动屏对应哪个模式；不在环上返回 STANDBY_MODE_COUNT。 */
standby_mode_t standby_ring_current(void);

/* 当前活动屏是否环上三页之一。 */
bool standby_ring_on_ring(void);

/* --------------------------------------------------------------------------
 *  表情模式显示哪一张（NVS 持久化）
 *
 *  ⚠ 写入方是**测试→Robot 页面**（app 上下文）那个按钮，**不是**表情模式本身。
 *  这两个页面是独立的两件事，用"壁纸商店 / 锁屏壁纸"的关系理解：
 *
 *    测试→Robot   = 商店：左滑翻看、右滑返回，单击出编辑层，
 *                   层里那个按钮调 standby_ring_set_emote_index() 把当前这张"放置"下来
 *    主页表情模式 = 锁屏：只读 standby_ring_emote_index() 显示那一张，不换表情、不出编辑层
 *
 *  ⚠ 这里**不做范围钳制**。合法范围要问 emote_gen_player_get_index_count()，
 *  而那需要先 mount 表情包分区 —— 在开机早期（甚至还没进过表情页时）不值得为
 *  一个查询去付这个代价。越界值是安全的：robot_app.c 的 play_emote() 本来就
 *  做 `index % count`，会自己绕回。
 * -------------------------------------------------------------------------- */

/* 选中的表情序号。首次调用时从 NVS 懒读，之后走内存缓存。读不到返回 0。 */
size_t standby_ring_emote_index(void);

/* 记下选中的表情序号并写 NVS。NVS 写失败时内存值仍然生效（只是不持久）。 */
void standby_ring_set_emote_index(size_t index);

#ifdef __cplusplus
}
#endif
