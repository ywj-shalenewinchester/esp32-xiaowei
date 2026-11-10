/*
 * imu_app.c —— IMU 测试页（测试 → IMU）
 *
 * 替代原来的"航空姿态仪"（ported/attitude_screen.c）。那个页面用 5 张 466×466 大图
 * 做纯软件旋转（本板没有 PPA 2D 加速），每 80ms 转一次，主观上就是"卡"；而它依赖的
 * xiaowei_imu_get() 当时还是个永远返回 false 的空桩，所以又"没什么作用"。
 *
 * 现在换成一屏对称滑条（lv_bar + LV_BAR_MODE_SYMMETRICAL）：器件一转动，条子从中间
 * 向左/右长出去，读数就在旁边。全部是纯色小控件，没有一张图，没有一次变换 ——
 * 每帧只重绘"变化了的那几个条子和数字"。
 *
 * 单位与量程：
 *   PITCH/ROLL  ±90°（加速度计估算：平放静止时两根都指 0）
 *   AX/AY/AZ    ±2000 mg（±2g 满量程）—— 平放静止时 AZ ≈ +1000
 *   GX/GY/GZ    ±250.0 dps（±250dps 满量程），显示保留一位小数
 * 角速度用"十分之一度"当整数单位，是为了绕开本工程的浮点 printf
 * （CONFIG_NEWLIB_NANO_FORMAT 下 %f 不可用，见 app_manager.c 的同款说明）。
 *
 * 滤波：7 路读数（6 个原始轴 + 温度）各走一遍一阶低通，见下面 IMU_TAU_*。
 *   PITCH/ROLL 是拿**滤波后**的加速度算的，所以它俩自动跟着一起平滑 ——
 *   不需要（也不该）再对角度本身滤第二遍。
 */
#include "imu_app.h"

#include <math.h>
#include <stdio.h>

#include "../../fonts.h"
#include "../../framework/app_manager.h"
#include "../../platform/xiaowei_ui_hw.h"

/* 采样周期。器件是 120Hz 出的数，20Hz 显示已经比眼睛快，且每帧只有一次
 * 20 字节左右的 I2C 读。 */
#define IMU_TICK_MS 50

/* ── 低通滤波（一阶 IIR，也就是指数滑动平均）──────────────────────────────
 *     滤波值 += alpha * (本次采样 - 滤波值)
 *     alpha   = 1 - exp(-IMU_TICK_MS / tau)
 *
 * 下面三个给的是**时间常数 tau**（毫秒），不是 alpha —— alpha 由它反算，
 * 所以以后改 IMU_TICK_MS，滤波强度不会跟着一起变。
 *
 * tau 怎么定的：想跟的信号（真在转设备、真在倾斜）都在 1~2Hz 以下；要压掉的
 * 东西（手抖 8~12Hz、器件本底噪声、20Hz 采样对 120Hz 输出的混叠）全在它之上，
 * 于是把拐点放在中间。
 *   加速度 150ms → 约 1Hz 以上开始压。AX/AY/AZ 平滑，姿态角拿它算，一起稳；
 *                  转设备仍然跟手（跳一下约 150ms 跟上 63%，350ms 到九成）。
 *   角速度 250ms → 比加速度更钝：手搁在设备上引起的微小晃动全吃掉，快速
 *                  甩一下仍然立刻起。
 *   温度   2s    → 纯粹是别让那个数字乱蹦，温度本来变得就慢。
 * 嫌跟着慢就把 tau 调小，嫌还在跳就调大。 */
#define IMU_TAU_ACC_MS   150
#define IMU_TAU_GYR_MS   250
#define IMU_TAU_TEMP_MS  2000

#define IMU_ANGLE_FULL   90    /* 度。atan2 的第二个参数恒为非负，所以只在 ±90 内 */
#define IMU_ACC_FULL     2000  /* mg */
#define IMU_GYR_FULL     2500  /* 十分之一度/秒 */

#define ROW_H      36
#define ROW_NAME_W 68
#define ROW_BAR_W  160
#define ROW_VAL_W  92

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *value;
    int full;          /* 条子量程 ±full（显示单位） */
    int tenths;        /* 非零 = 读数带一位小数（值本身是十倍） */
    int last;          /* 上次画上去的值，相同就整行跳过 */
    int valid;
} imu_row_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_timer_t *s_timer;
static imu_row_t s_rows[8];
/* -1 未知（还没读第一次）、0 离线、1 在线。只在状态切换的那一帧动控件，
 * 免得器件掉线后每 50ms 把八个条子和读数全部重绘一遍。 */
static int s_online = -1;
static uint32_t s_status_ms;   /* 状态行上次刷新时刻；0 = 立刻刷新 */

/* 滤波状态。v[] 的下标和解包顺序一致：0~2 是加速度(mg)，3~5 是角速度(mdps)。 */
static float s_lpf_v[6];
static float s_lpf_temp;
static bool  s_lpf_primed;     /* 未收敛 = 下一个采样直接当初值，不从 0 爬 */
static float s_alpha_acc, s_alpha_gyr, s_alpha_temp;

enum { ROW_PITCH, ROW_ROLL, ROW_AX, ROW_AY, ROW_AZ, ROW_GX, ROW_GY, ROW_GZ };

static float imu_alpha(int tau_ms)
{
    return 1.0f - expf(-(float)IMU_TICK_MS / (float)tau_ms);
}

/* 建页面时算一次。expf 不在采样路径上，无所谓快慢。 */
static void imu_filter_init(void)
{
    s_alpha_acc  = imu_alpha(IMU_TAU_ACC_MS);
    s_alpha_gyr  = imu_alpha(IMU_TAU_GYR_MS);
    s_alpha_temp = imu_alpha(IMU_TAU_TEMP_MS);
    s_lpf_primed = false;
}

/* 原地把 7 个读数换成滤波后的值。 */
static void imu_filter(float *ax, float *ay, float *az,
                       float *gx, float *gy, float *gz, float *temp)
{
    float *raw[6] = { ax, ay, az, gx, gy, gz };

    if (!s_lpf_primed) {
        /* 首次收敛（开机后第一次、每次进页面、器件掉线重连）直接取原始值当
           初值。若从 0 开始指数爬，看着就像"设备在慢慢立起来"，跟真正的滤波
           滞后混在一起分不清。 */
        for (int i = 0; i < 6; ++i) s_lpf_v[i] = *raw[i];
        s_lpf_temp = *temp;
        s_lpf_primed = true;
    } else {
        for (int i = 0; i < 6; ++i) {
            const float a = (i < 3) ? s_alpha_acc : s_alpha_gyr;
            s_lpf_v[i] += a * (*raw[i] - s_lpf_v[i]);
        }
        s_lpf_temp += s_alpha_temp * (*temp - s_lpf_temp);
    }

    *ax = s_lpf_v[0]; *ay = s_lpf_v[1]; *az = s_lpf_v[2];
    *gx = s_lpf_v[3]; *gy = s_lpf_v[4]; *gz = s_lpf_v[5];
    *temp = s_lpf_temp;
}

static void imu_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) return;
    xiaowei_app_back();
}

/* 读数文本。tenths 的行手写小数点：本工程没有浮点 printf。 */
static void format_value(char *buf, size_t n, const imu_row_t *row, int v)
{
    if (row->tenths) {
        int a = v < 0 ? -v : v;
        snprintf(buf, n, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
    } else {
        snprintf(buf, n, "%d", v);
    }
}

static void row_set(imu_row_t *row, int v)
{
    if (v > row->full) v = row->full;
    if (v < -row->full) v = -row->full;
    if (row->valid && v == row->last) return;   /* 数值没变，不碰控件 */
    row->last = v;
    row->valid = 1;

    lv_bar_set_value(row->bar, v, LV_ANIM_OFF);

    char buf[16];
    format_value(buf, sizeof(buf), row, v);
    lv_label_set_text(row->value, buf);
}

static void add_row(lv_obj_t *parent, int idx, const char *name, int full,
                    int tenths, uint32_t color)
{
    imu_row_t *row = &s_rows[idx];
    row->full = full;
    row->tenths = tenths;
    row->valid = 0;

    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), ROW_H);
    /* 整行不接受点击：右滑返回的手势要靠事件冒泡传到屏幕对象上。 */
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(line);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xC9D1D9), LV_PART_MAIN);
    lv_obj_set_size(label, ROW_NAME_W, ROW_H);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    /* 对称模式：零值从正中起画，正负一目了然 —— 这就是"转动时左右增减"的观感来源。 */
    lv_obj_t *bar = lv_bar_create(line);
    lv_obj_set_size(bar, ROW_BAR_W, 14);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, ROW_NAME_W, 0);
    lv_bar_set_mode(bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_range(bar, -full, full);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x39424C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    row->bar = bar;

    row->value = lv_label_create(line);
    lv_label_set_text(row->value, "-");
    lv_obj_set_style_text_font(row->value, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(row->value, lv_color_hex(0xF9F5F5), LV_PART_MAIN);
    lv_obj_set_style_text_align(row->value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_size(row->value, ROW_VAL_W, ROW_H);
    lv_obj_align(row->value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void imu_build(void)
{
    imu_filter_init();

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 466, 466);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, imu_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "IMU");
    lv_obj_set_style_text_font(title, &ui_font_font_32_cn, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF9F5F5), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    /* 圆屏安全区（和旧工程二级页一致的口径）：x=73 起、宽 320。 */
    lv_obj_t *column = lv_obj_create(s_screen);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 320, 8 * ROW_H + 7 * 3);
    lv_obj_set_pos(column, 73, 84);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(column, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(column, 3, LV_PART_MAIN);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    /* 姿态角在前：这一组最能直接对应"我把设备转了一下"。 */
    add_row(column, ROW_PITCH, "PITCH", IMU_ANGLE_FULL, 0, 0xE8A33D);
    add_row(column, ROW_ROLL,  "ROLL",  IMU_ANGLE_FULL, 0, 0xE8A33D);
    add_row(column, ROW_AX, "AX", IMU_ACC_FULL, 0, 0x2F6FED);
    add_row(column, ROW_AY, "AY", IMU_ACC_FULL, 0, 0x2F6FED);
    add_row(column, ROW_AZ, "AZ", IMU_ACC_FULL, 0, 0x2F6FED);
    add_row(column, ROW_GX, "GX", IMU_GYR_FULL, 1, 0x33C46B);
    add_row(column, ROW_GY, "GY", IMU_GYR_FULL, 1, 0x33C46B);
    add_row(column, ROW_GZ, "GZ", IMU_GYR_FULL, 1, 0x33C46B);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "-");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static void imu_tick(lv_timer_t *timer)
{
    (void)timer;

    float ax, ay, az, gx, gy, gz, temp;
    if (!xiaowei_imu_get(&ax, &ay, &az, &gx, &gy, &gz, &temp)) {
        /* 读不到就退回空屏并写明原因 —— 不保留上一次的旧值假装还在动。 */
        if (s_online != 0) {
            s_online = 0;
            s_lpf_primed = false;   /* 重连后第一帧直接收敛，不拿掉线前的旧值爬 */
            for (int i = 0; i < 8; ++i) {
                s_rows[i].valid = 0;
                lv_bar_set_value(s_rows[i].bar, 0, LV_ANIM_OFF);
                lv_label_set_text(s_rows[i].value, "-");
            }
            lv_label_set_text(s_status, "IMU OFFLINE");
            s_status_ms = 0;
        }
        return;
    }
    s_online = 1;

    /* 先滤波，再算别的 —— 下面每一行（含姿态角）用的都是滤波后的值。 */
    imu_filter(&ax, &ay, &az, &gx, &gy, &gz, &temp);

    /* 姿态角：只用加速度计。取"平放 = 0°"这一套（原姿态仪用的是 atan2(-az, ...)，
       那套里平放读 -90°，对着它的刻度盘合适，对着滑条不合适）。
       器件在板上的实际朝向若与此不同，只改这两行的轴和符号即可 —— 反正旁边
       就是 AX/AY/AZ 的原始读数，对着看就知道该换哪个。 */
    const float RAD2DEG = 57.2958f;
    float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD2DEG;
    float roll  = atan2f( ay, sqrtf(ax * ax + az * az)) * RAD2DEG;

    row_set(&s_rows[ROW_PITCH], (int)lroundf(pitch));
    row_set(&s_rows[ROW_ROLL],  (int)lroundf(roll));
    row_set(&s_rows[ROW_AX], (int)lroundf(ax * 1000.0f));
    row_set(&s_rows[ROW_AY], (int)lroundf(ay * 1000.0f));
    row_set(&s_rows[ROW_AZ], (int)lroundf(az * 1000.0f));
    row_set(&s_rows[ROW_GX], (int)lroundf(gx * 10.0f));
    row_set(&s_rows[ROW_GY], (int)lroundf(gy * 10.0f));
    row_set(&s_rows[ROW_GZ], (int)lroundf(gz * 10.0f));

    /* 状态行 1 秒最多改一次：它是纯文字，改了就要重绘那一小块。 */
    uint32_t now = lv_tick_get();
    if (s_status_ms == 0 || (uint32_t)(now - s_status_ms) >= 1000) {
        s_status_ms = now;
        char buf[24];
        snprintf(buf, sizeof(buf), "TEMP %dC", (int)lroundf(temp));
        lv_label_set_text(s_status, buf);
    }
}

static lv_obj_t *imu_create(void)
{
    if (!s_screen) imu_build();
    return s_screen;
}

/* 定时器只在页面可见时存在：离开页面就没必要每 50ms 去敲一次 I2C。 */
static void imu_on_open(void)
{
    /* 页面不可见时定时器是停的，滤波状态停在上次离开那一刻。置成未收敛，
       让这一帧直接用当前读数当初值，而不是拿几分钟前的旧值慢慢爬过来。 */
    s_lpf_primed = false;
    if (!s_timer) s_timer = lv_timer_create(imu_tick, IMU_TICK_MS, NULL);
    imu_tick(s_timer);
}

static void imu_on_close(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static const xiaowei_app_t s_descriptor = {
    .id = "imu_test", .title = "IMU", .icon = NULL,
    .create_screen = imu_create, .on_open = imu_on_open, .on_close = imu_on_close,
    .destroy_screen = NULL
};

const xiaowei_app_t *imu_app_descriptor(void) { return &s_descriptor; }
