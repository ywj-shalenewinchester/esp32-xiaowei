/*
 * simon_game.h
 * Simon Says 记忆游戏（移植自 Chronos esp32-c3-mini 的 games/simon，MIT License）
 * 游戏本体纯 LVGL 控件，无图片依赖；蜂鸣音效在本工程为空实现。
 * 最高分通过 NVS 持久化（namespace "simon", key "high"）。
 */
#ifndef PORTED_SIMON_GAME_H
#define PORTED_SIMON_GAME_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 Simon 游戏屏幕（主菜单"游戏"项点击调用） */
void simon_game_open(lv_event_t *e);
lv_obj_t *simon_game_create_for_app(void);

#ifdef __cplusplus
}
#endif

#endif /* PORTED_SIMON_GAME_H */
