#include "audio_app.h"
#include "../../platform/xiaowei_ui_hw.h"
#include "../../framework/app_manager.h"

/* ==========================================================================
 *  录音回放测试页
 *
 *  这一页的定位是**诊断工具**，不只是"能录音能放音"：麦克风出不出声、增益够不够、
 *  唤醒词挂起/恢复对不对，全都能在这一页上看见。所以除了 START/STOP，还画了一根
 *  电平条和一行原始电平读数。
 *
 *  数据来源见 platform/xiaowei_ui_hw.cc。音频底层一次给出两组电平：
 *    RAW = PDM 解调 + 去直流之后、**加增益之前** —— 反映麦克风本身的灵敏度；
 *    AFE = **加增益之后**，真正送进唤醒词/ASR 的 —— 反映这条链路送到模型面前有多响。
 *  电平条画 AFE（不然调 MIC_GAIN 时条子纹丝不动，等于把增益调没了），
 *  两组数字并排显示：静音时 AFE 停在哪儿，就代表信噪比还剩多少。
 *
 *  ⚠ 定时器不只是刷界面：xiaowei_audio_busy() 每次调用都是一个收尾检查点，
 *   整轮测试走完后由它把录音期间摘掉的唤醒词装回去。所以即使页面不在前台，
 *   也**必须**照常调它（下面就是这么做的）。
 * ========================================================================== */

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_level_text;

/* 电平条满量程 —— 量的是**加增益之后**、真正喂给 AFE 的值。
 *
 * 6000 取自厂商参考工程的目标响度（audio_test.c:70 的 VOICE_GAIN_TARGET_RMS）：
 * 那边就是把这颗麦拉到 RMS 6000 当作"响亮"。所以正常说话占到条的一半上下是对的；
 * 而**静音时条子停在哪儿**才是这一页最该看的东西 —— 它直接告诉你信噪比还剩多少，
 * 也就决定了继续抬 MIC_GAIN 还有没有意义（底噪跟着一起抬就说明没有）。
 *
 * 加增益之前的原始值只作为旁边那行数字显示，不画条：它不随 MIC_GAIN 变，
 * 拿它画条等于把增益调没了。 */
#define AUDIO_LEVEL_FULL_SCALE 6000

static void audio_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        /* 不用在这里停录：xiaowei_app_back() 会调 on_close，停录在那里做。
           两处都停也没关系（record_stop 是幂等的），但只留一处更不容易漏。 */
        xiaowei_app_back();
    }
}

static void audio_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_status) return;
    intptr_t id = (intptr_t)lv_event_get_user_data(event);
    if (id == 0) {
        lv_label_set_text(s_status, xiaowei_audio_record_start() ?
                          "RECORDING..." : "BUSY / NOT READY");
    } else if (xiaowei_audio_busy()) {
        lv_label_set_text(s_status, "STOP -> PLAYBACK...");
        xiaowei_audio_record_stop();
    } else {
        lv_label_set_text(s_status, "PRESS START FIRST");
    }
}

static void add_audio_button(const char *text, lv_color_t color, intptr_t id,
                             lv_align_t align, int x, int y)
{
    lv_obj_t *button = lv_button_create(s_screen);
    lv_obj_set_size(button, 140, 70);
    lv_obj_align(button, align, x, y);
    lv_obj_add_event_cb(button, audio_button_event, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_center(label);
}

/* --------------------------------------------------------------------------
 *  200ms 轮询：电平条 + 状态 + 唤醒词收尾
 * -------------------------------------------------------------------------- */
static void audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 顺序有讲究：先问 recording 再问 busy。
       过了框架那 10 秒上限之后 recording 会落下，而 busy 还因为回放在继续而保持为真，
       两者一对比就知道现在是录还是放。 */
    const bool recording = xiaowei_audio_recording();
    const bool busy = xiaowei_audio_busy();

    /* 页面不在前台时只做上面那次 busy 调用（它负责唤醒词收尾），不碰 LVGL 对象 ——
       定时器是建在屏上的常驻对象，用户早就滑走了它也还在跑。 */
    if (lv_screen_active() != s_screen || s_status == NULL) return;

    /* 状态文字由真实状态反推，而不是由"按过哪个按钮"记忆 —— 否则框架自己停录转回放
       （录满 10 秒）之后，界面会一直停在 RECORDING... */
    if (!busy) {
        lv_label_set_text(s_status, "PRESS START TO RECORD");
    } else if (recording) {
        lv_label_set_text(s_status, "RECORDING...");
    } else {
        lv_label_set_text(s_status, "PLAYBACK...");
    }

    int rms = 0, peak = 0, afe_rms = 0, afe_peak = 0;
    xiaowei_audio_input_levels(&rms, &peak);
    xiaowei_audio_afe_levels(&afe_rms, &afe_peak);
    if (s_level_bar) {
        int shown = afe_rms;
        if (shown > AUDIO_LEVEL_FULL_SCALE) shown = AUDIO_LEVEL_FULL_SCALE;
        lv_bar_set_value(s_level_bar, shown, LV_ANIM_OFF);
    }
    if (s_level_text) {
        /* 两组都照实显示（不截断）—— 这是拿来调增益的数字，截断了就没用了。
           RAW = 加增益之前（麦克风本身的灵敏度，图省地方省略单位）
           AFE = 加增益之后（真正送进唤醒词/ASR 的），条子画的就是它。 */
        lv_label_set_text_fmt(s_level_text, "RAW %d/%d   AFE %d/%d",
                              rms, peak, afe_rms, afe_peak);
    }
}

static void audio_create_impl(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 466, 466);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, audio_event, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "AUDIO REC TEST");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    /* 电平条：量程 0..AUDIO_LEVEL_FULL_SCALE，整数步进。
       加了 LV_OBJ_FLAG_CLICKABLE 之外什么都不用 —— 它纯显示，不接事件。 */
    s_level_bar = lv_bar_create(s_screen);
    lv_obj_set_size(s_level_bar, 300, 20);
    lv_obj_align(s_level_bar, LV_ALIGN_TOP_MID, 0, 76);
    lv_bar_set_range(s_level_bar, 0, AUDIO_LEVEL_FULL_SCALE);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_level_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x60FF90), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_level_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 4, LV_PART_INDICATOR);

    s_level_text = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_level_text, lv_color_hex(0x808080), 0);
    lv_obj_align(s_level_text, LV_ALIGN_TOP_MID, 0, 104);
    lv_label_set_text(s_level_text, "RAW 0/0   AFE 0/0");

    add_audio_button("START", lv_color_hex(0xFF6060), 0,
                     LV_ALIGN_LEFT_MID, 46, 30);
    add_audio_button("STOP", lv_color_hex(0x60FF90), 1,
                     LV_ALIGN_RIGHT_MID, -46, 30);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xA0A0A0), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -46);

    /* 屏是常驻单例、从不销毁，所以定时器只建一次，也不需要 on_delete 回调去收它。
       （和 robot_app.c 的做法一致。） */
    lv_timer_create(audio_timer_cb, 200, NULL);
}

static lv_obj_t *audio_create(void)
{
    if (!s_screen) audio_create_impl();
    lv_label_set_text(s_status, "PRESS START TO RECORD");
    return s_screen;
}

/* 离开页面（右滑返回、或者被语音打断让位给聊天页）都要停录。
   停录这一下同时是"转入回放"，另外还会让底层去等设备空闲后恢复唤醒词 ——
   所以哪怕已经停了也要调，它是幂等的。 */
static void audio_close(void)
{
    xiaowei_audio_record_stop();
}

static const xiaowei_app_t s_desc = {
    .id = "audio_test", .title = "audio", .icon = NULL,
    .create_screen = audio_create, .on_open = NULL, .on_close = audio_close,
    .destroy_screen = NULL,
};

const xiaowei_app_t *audio_test_app_descriptor(void) { return &s_desc; }
