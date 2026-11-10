/*
 * simon_game.c
 * Simon Says 记忆游戏（移植自 Chronos esp32-c3-mini 的 games/simon，MIT License）
 *
 * 玩法：设备按顺序点亮四色面板，玩家依次复现，每轮序列 +1，最长 50 关。
 * 适配：原版 240x240 -> 本工程 466x466（面板 400x400，色块 200x200）。
 * 音效：原版 simonTone() 走蜂鸣器，本工程暂无蜂鸣器，保留为空函数。
 * 高分：NVS namespace "simon" / key "high"。
 *
 * 与 lcd 工程的唯一差异：LVGL v8 的 lv_obj_set_hidden() / lv_obj_is_hidden()
 * 在 v9 里已经删掉（本工程是 9.5.0，头文件里没有兼容别名），换成 v9 的 flag
 * 接口，语义一一对应：
 *     lv_obj_set_hidden(o, true)  -> lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN)
 *     lv_obj_set_hidden(o, false) -> lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN)
 *     lv_obj_is_hidden(o)         -> lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)
 * 全文件共 10 处。
 */
#include "simon_game.h"

#include <stdint.h>

#include "../assets/xiaowei_assets.h"
#include "../framework/app_manager.h"
#include "../shell/submenu.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define SIMON_NUM_PANELS       4
#define SIMON_MAX_SEQUENCE     50
#define SIMON_BOARD_SIZE       400    /* 游戏母板边长（四宫格外轮廓） */
#define SIMON_PANEL_SIZE       200    /* 单个色块边长 */
#define SIMON_CENTER_SIZE      100    /* 中央计分圆直径 */

static const char *TAG = "SIMON";

/* 四色音调（保留，当前无蜂鸣器）与颜色 */
static const uint16_t s_tones[SIMON_NUM_PANELS] = { 440, 494, 554, 622 };
static const uint32_t s_panel_colors[SIMON_NUM_PANELS] = {
    0x00801F, 0xD20000, 0xCDED07, 0x0A36E8
};

static int s_sequence[SIMON_MAX_SEQUENCE];
static int s_current_step;
static int s_sequence_index;
static int s_sequence_length = 1;
static bool s_player_turn = false;
static int s_high_score;

static lv_obj_t *s_screen;
static lv_obj_t *s_panels[SIMON_NUM_PANELS];
static lv_obj_t *s_score_panel;
static lv_obj_t *s_score_text;
static lv_obj_t *s_arc;
static lv_obj_t *s_over_panel;      /* 轮次切换提示遮罩（"看好了"） */
static lv_obj_t *s_menu_panel;      /* 开始/结束菜单面板 */
static lv_obj_t *s_title;
static lv_obj_t *s_label;
static lv_obj_t *s_info;
static lv_obj_t *s_score;
static lv_obj_t *s_start_btn_text;

typedef struct {
    const char *title;
    const char *message;
} score_info_t;

static const score_info_t s_levels[] = {
    {"Beginner",    "You are just starting. Keep practicing!"},
    {"Learner",     "You're getting the hang of it!"},
    {"Reminiscer",  "You're recalling some details now!"},
    {"Frequent",    "You're improving with regular attempts!"},
    {"Intermediate","You're halfway there. Stay focused!"},
    {"Reliable",    "You're consistent with your memory!"},
    {"Proficient",  "You're handling things with confidence!"},
    {"Sharp",       "Your memory is sharp and reliable!"},
    {"Expert",      "You've mastered most of the challenges!"},
    {"Master",      "You're a memory master. Exceptional work!"},
    {"Champion",    "The G.O.A.T! You completed the whole sequence!"}
};

/* ---------------- 音效（空实现，可接 ES8311 播放 beep） ---------------- */
static void simon_tone(int type, int pitch)
{
    (void)type;
    (void)pitch;
}

/* ---------------- NVS 最高分 ----------------
 * 系统启动时已执行 nvs_flash_init（见 main.cpp），这里直接 open 即可；
 * namespace 尚不存在时 READONLY 打开失败，按 0 分处理。 */
static int simon_high_load(void)
{
    nvs_handle_t h;
    if (nvs_open("simon", NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t v = 0;
    nvs_get_i32(h, "high", &v);
    nvs_close(h);
    return (int)v;
}

static void simon_high_save(int v)
{
    nvs_handle_t h;
    if (nvs_open("simon", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "high", v);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 游戏逻辑 ---------------- */
static void play_sequence(lv_timer_t *timer);

/* 闪烁是严格串行的（500ms 定时链），当前音调只需一个静态变量 */
static uint16_t s_flash_tone;

static void remove_state_cb(lv_timer_t *timer)
{
    lv_obj_t *panel = (lv_obj_t *)lv_timer_get_user_data(timer);
    lv_obj_remove_state(panel, LV_STATE_PRESSED);
    play_sequence(NULL);
}

static void add_state_cb(lv_timer_t *timer)
{
    lv_obj_t *panel = (lv_obj_t *)lv_timer_get_user_data(timer);
    lv_obj_add_state(panel, LV_STATE_PRESSED);

    lv_timer_t *remove_t = lv_timer_create(remove_state_cb, 500, panel);
    lv_timer_set_repeat_count(remove_t, 1);
    lv_timer_set_auto_delete(remove_t, true);

    simon_tone(2, s_flash_tone);
}

static void flash_panel(int panel_idx)
{
    s_flash_tone = s_tones[panel_idx];
    lv_timer_t *timer = lv_timer_create(add_state_cb, 500, s_panels[panel_idx]);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_auto_delete(timer, true);
}

static void play_sequence(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_remove_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(s_arc, 360);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xCCFFF9), LV_PART_INDICATOR);

    lv_label_set_text_fmt(s_score_text, "%d", s_sequence_length);
    s_player_turn = false;
    if (s_sequence_index < s_sequence_length) {
        flash_panel(s_sequence[s_sequence_index]);
        s_sequence_index++;
    } else {
        s_sequence_index = 0;
        s_player_turn = true;
        s_current_step = 0;
        lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(s_arc, 0);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2A2A2A), LV_PART_INDICATOR);
    }
}

static void generate_sequence(void)
{
    for (int i = 0; i < SIMON_MAX_SEQUENCE; i++)
        s_sequence[i] = lv_rand(0, SIMON_NUM_PANELS - 1);
}

static void start_game(void)
{
    generate_sequence();
    s_current_step = 0;
    s_sequence_index = 0;
    s_sequence_length = 1;
    play_sequence(NULL);
}

static void game_over_panel(const char *title)
{
    int reached = s_sequence_length - 1;
    lv_label_set_text_fmt(s_score, "%d/%d", reached, SIMON_MAX_SEQUENCE);
    lv_label_set_text(s_label, title);
    int idx = reached / (SIMON_MAX_SEQUENCE / 10);
    if (idx > 10) idx = 10;
    lv_label_set_text(s_info, s_levels[idx].message);
    lv_obj_remove_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_start_btn_text, "Start");
    if (reached > s_high_score) {
        s_high_score = reached;
        simon_high_save(s_high_score);
    }
}

static void panel_event_handler(lv_event_t *e)
{
    if (!s_player_turn) return;

    int panel_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (panel_idx != s_sequence[s_current_step]) {
        simon_tone(1, 0);
        game_over_panel("Game Over!");
        return;
    }

    simon_tone(2, s_tones[panel_idx]);
    s_current_step++;

    lv_arc_set_value(s_arc, (int32_t)((s_current_step * 360.0) / s_sequence_length));
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x50FF00), LV_PART_INDICATOR);

    if (s_current_step == s_sequence_length) {
        if (s_sequence_length == SIMON_MAX_SEQUENCE) {
            s_high_score = s_sequence_length;
            simon_high_save(s_high_score);
            lv_label_set_text_fmt(s_score, "%d", s_high_score);
            game_over_panel(s_levels[10].title);
        } else {
            s_sequence_length++;
            lv_obj_remove_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_value(s_arc, 360);
            lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x50FF00), LV_PART_INDICATOR);
            lv_timer_t *next_round = lv_timer_create(play_sequence, 1000, NULL);
            lv_timer_set_repeat_count(next_round, 1);
            lv_timer_set_auto_delete(next_round, true);
        }
    }
}

/* ---------------- 屏幕与控件 ---------------- */

/* GAMES 二级菜单：列出游戏英文名，点击后才进入对应游戏 */
static lv_obj_t *s_games_menu;

static void games_menu_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        xiaowei_app_back();
    }
}

/* 点击 "SIMON SAYS" 条目：进入游戏屏（游戏屏的 LOAD_START 事件会重置面板） */
static void simon_open_game(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ESP_LOGI(TAG, "item clicked -> load game screen");
    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

/* 游戏二级菜单的条目表。和相机那份共用 shell/submenu（也就是「测试/工具」
   那几页的 lv_list 形式）：图标在左、英文在右；多一个游戏就往这里加一行。 */
static const xiaowei_submenu_item_t s_game_menu_items[] = {
    { &xiaowei_asset_icon_game, "SIMON SAYS", simon_open_game, NULL },
};

static void start_btn_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    start_game();
}

static void exit_btn_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_player_turn = false;
    lv_scr_load_anim(s_games_menu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
}

static void screen_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOAD_START) {
        /* 每次进入恢复菜单面板（屏幕对象常驻复用，不销毁） */
        ESP_LOGI(TAG, "LOAD_START: high load");
        s_high_score = simon_high_load();
        ESP_LOGI(TAG, "LOAD_START: high=%d, reset labels", s_high_score);
        lv_label_set_text(s_start_btn_text, "Start");
        lv_label_set_text(s_label, "High Score");
        lv_label_set_text(s_info, "Watch the sequence,\nthen repeat it.");
        lv_label_set_text(s_score_text, "0");
        lv_label_set_text_fmt(s_score, "%d", s_high_score);
        lv_obj_remove_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
        s_player_turn = false;
        ESP_LOGI(TAG, "LOAD_START done");
    } else if (code == LV_EVENT_GESTURE &&
               lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        /* 游戏中右滑 = 暂停回菜单；菜单态右滑 = 退出回游戏列表 */
        if (lv_obj_has_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN)) {
            s_player_turn = false;
            lv_obj_remove_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_start_btn_text, "Resume");
        } else {
            lv_scr_load_anim(s_games_menu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
        }
    }
}

static lv_obj_t *make_menu_button(lv_obj_t *parent, const char *text, uint32_t bg_color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 180, 64);
    lv_obj_set_style_radius(btn, 32, LV_PART_MAIN);
    if (bg_color) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

void simon_game_open(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "open: enter (s_screen=%p)", (void *)s_screen);
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        ESP_LOGI(TAG, "open: screen created");
        lv_obj_set_size(s_screen, 466, 466);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

        /* 四色母板 */
        lv_obj_t *board = lv_obj_create(s_screen);
        /* remove_style_all 必须在 flex 等样式设置之前，否则会被清空；
           同时去掉默认内边距，保证四色块恰好拼成 2x2 */
        lv_obj_remove_style_all(board);
        lv_obj_set_size(board, SIMON_BOARD_SIZE, SIMON_BOARD_SIZE);
        lv_obj_center(board);
        lv_obj_set_flex_flow(board, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(board, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < SIMON_NUM_PANELS; i++) {
            s_panels[i] = lv_obj_create(board);
            lv_obj_set_size(s_panels[i], SIMON_PANEL_SIZE, SIMON_PANEL_SIZE);
            lv_obj_clear_flag(s_panels[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_radius(s_panels[i], 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_panels[i], lv_color_hex(s_panel_colors[i]),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(s_panels[i], 150, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(s_panels[i], lv_color_black(),
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(s_panels[i], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            /* 按下（点亮）态：全不透明 + 白边 */
            lv_obj_set_style_bg_color(s_panels[i], lv_color_hex(s_panel_colors[i]),
                                      LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(s_panels[i], 255, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_border_color(s_panels[i], lv_color_white(),
                                          LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_border_width(s_panels[i], 1, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_add_event_cb(s_panels[i], panel_event_handler, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
        ESP_LOGI(TAG, "open: 4 panels created");

        /* 中央计分圆 + 进度弧 */
        s_score_panel = lv_obj_create(s_screen);
        lv_obj_set_size(s_score_panel, SIMON_CENTER_SIZE, SIMON_CENTER_SIZE);
        lv_obj_center(s_score_panel);
        lv_obj_clear_flag(s_score_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_score_panel, SIMON_CENTER_SIZE / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_score_panel, lv_color_hex(0x171717), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_score_panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_outline_width(s_score_panel, 5, LV_PART_MAIN);
        lv_obj_set_style_outline_color(s_score_panel, lv_color_black(), LV_PART_MAIN);

        s_score_text = lv_label_create(s_score_panel);
        lv_label_set_text(s_score_text, "0");
        lv_obj_set_style_text_font(s_score_text, &lv_font_montserrat_30, LV_PART_MAIN);
        lv_obj_center(s_score_text);

        s_arc = lv_arc_create(s_score_panel);
        lv_obj_set_size(s_arc, SIMON_CENTER_SIZE, SIMON_CENTER_SIZE);
        lv_obj_center(s_arc);
        lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_range(s_arc, 0, 360);
        lv_arc_set_value(s_arc, 360);
        lv_arc_set_bg_angles(s_arc, 0, 360);
        lv_arc_set_rotation(s_arc, 270);
        lv_obj_set_style_arc_width(s_arc, 5, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_arc, 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xD2D2FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_arc, 0, LV_PART_KNOB);

        /* 轮次切换遮罩（播放序列时盖住色块防误触） */
        s_over_panel = lv_obj_create(s_screen);
        lv_obj_set_size(s_over_panel, SIMON_BOARD_SIZE, SIMON_BOARD_SIZE);
        lv_obj_center(s_over_panel);
        lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_over_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_style_all(s_over_panel);
        lv_obj_set_style_bg_opa(s_over_panel, LV_OPA_TRANSP, LV_PART_MAIN);

        /* 开始/结束菜单面板 */
        s_menu_panel = lv_obj_create(s_screen);
        lv_obj_set_size(s_menu_panel, SIMON_BOARD_SIZE, SIMON_BOARD_SIZE);
        lv_obj_center(s_menu_panel);
        lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_menu_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_menu_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_radius(s_menu_panel, 24, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_menu_panel, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_menu_panel, 235, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_menu_panel, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(s_menu_panel, 20, LV_PART_MAIN);

        s_title = lv_label_create(s_menu_panel);
        lv_label_set_text(s_title, "Simon Says");
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_title, lv_color_white(), LV_PART_MAIN);

        s_label = lv_label_create(s_menu_panel);
        lv_label_set_text(s_label, "High Score");
        lv_obj_set_style_text_font(s_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_label, lv_color_white(), LV_PART_MAIN);

        s_score = lv_label_create(s_menu_panel);
        lv_label_set_text(s_score, "0");
        lv_obj_set_style_text_font(s_score, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_score, lv_color_hex(0x18F80E), LV_PART_MAIN);

        s_info = lv_label_create(s_menu_panel);
        lv_obj_set_width(s_info, 320);
        lv_label_set_text(s_info, "Watch the sequence,\nthen repeat it.");
        lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_info, lv_color_hex(0xC0C0C0), LV_PART_MAIN);

        lv_obj_t *start_btn = make_menu_button(s_menu_panel, "Start", 0x2F6FED);
        s_start_btn_text = lv_obj_get_child(start_btn, 0);
        lv_obj_add_event_cb(start_btn, start_btn_event, LV_EVENT_CLICKED, NULL);

        lv_obj_t *exit_btn = make_menu_button(s_menu_panel, "Exit", 0xF30000);
        lv_obj_add_event_cb(exit_btn, exit_btn_event, LV_EVENT_CLICKED, NULL);

        lv_obj_add_event_cb(s_screen, screen_event, LV_EVENT_ALL, NULL);

        ESP_LOGI(TAG, "simon screen created (board=%d)", SIMON_BOARD_SIZE);
    }

    /* 游戏二级菜单：标题 + 游戏条目，整批交给 shell/submenu —— 就是
       「测试/工具」那几页的 lv_list 形式（原来这里的标题、条目、">" 提示
       都是现搓的，条目还没有图标，和别处长得完全不一样）。 */
    if (!s_games_menu) {
        s_games_menu = lv_obj_create(NULL);
        lv_obj_set_size(s_games_menu, 466, 466);
        lv_obj_clear_flag(s_games_menu, LV_OBJ_FLAG_SCROLLABLE);
        /* 底色和「测试/工具/AI助手」那几页一致（legacy_app.c 的 style_screen）。 */
        lv_obj_set_style_bg_color(s_games_menu, lv_color_hex(0x101418), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_games_menu, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_event_cb(s_games_menu, games_menu_event, LV_EVENT_GESTURE, NULL);
        xiaowei_submenu_build(s_games_menu, "游戏", s_game_menu_items,
                              sizeof(s_game_menu_items) /
                                  sizeof(s_game_menu_items[0]));

        ESP_LOGI(TAG, "games menu created");
    }

    /* 主菜单"游戏"先进入游戏列表；具体游戏由列表条目进入 */
    lv_scr_load_anim(s_games_menu, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

lv_obj_t *simon_game_create_for_app(void)
{
    simon_game_open(NULL);
    return s_games_menu;
}
