#include "low_ui.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <time.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "device_state.h"
#include "display/display.h"
#include "display/lvgl_display/lvgl_theme.h"

/* 三张 90x90 的 ARGB8888 图标（音量 / 亮度 / 天气）原本只给 P4 的
 * display/src/ui/ 用；这次在 CMake 里把它们单独编进低配版。 */
#include "xiaowei_assets.h"

static const char *TAG = "LowUi";

namespace {

constexpr int kPanelW = 360;
constexpr int kPanelH = 360;
constexpr int kSlideMs = 160;

constexpr uint32_t kBgColor = 0x0B0E12;
constexpr uint32_t kTileColor = 0x20262E;
constexpr uint32_t kAccentColor = 0xFFB74D;
constexpr uint32_t kTextColor = 0xF2F4F6;
constexpr uint32_t kMutedColor = 0x9AA4AE;

/* 菜单五格：音量、亮度、二维码、天气、关闭。 */
enum Page {
    kPageClosed = 0,
    kPageMenu,
    kPageVolume,
    kPageBrightness,
    kPageQr,
    kPageWeather,
    kPageCount,
};

constexpr int kMenuTileCount = 5;
constexpr int kAdjustButtonCount = 3;  // − / + / 返回
constexpr int kWeatherDayCount = 7;    // 服务端 daily 最多 7 条，正好一天一屏
constexpr int kWeatherButtonCount = 2;  // 下一天 / 返回

constexpr int kWeatherNoValue = -1000;
constexpr int kBrightnessMin = 10;  // 低于 10 屏幕基本全黑，用户没法再操作

Display *s_display = nullptr;
lv_obj_t *s_panel = nullptr;
lv_obj_t *s_pages[kPageCount] = {};
Page s_page = kPageClosed;
lv_timer_t *s_auto_close_timer = nullptr;
/* 上次看到的设备状态。只在“从非语音态跳进语音态”时才自动收起，
   这样在 Listening 里主动呼出菜单不会被定时器立刻关掉。 */
DeviceState s_last_state = kDeviceStateUnknown;

lv_obj_t *s_menu_tiles[kMenuTileCount] = {};
int s_menu_sel = 0;

/* 音量页和亮度页结构完全一样，共用一个描述体。 */
struct AdjustPage {
    lv_obj_t *page = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *buttons[kAdjustButtonCount] = {};
    int sel = 0;
    int value_now = 0;
};

AdjustPage s_volume;
AdjustPage s_brightness;

lv_obj_t *s_qr_back = nullptr;

/* 服务端 artifact_download 弹出的全屏下载码。独立于按键菜单，
   直接盖在 lv_layer_top() 上；用户扫码后超时自动收起。 */
lv_obj_t *s_download_qr_page = nullptr;
lv_obj_t *s_download_qr_code = nullptr;
esp_timer_handle_t s_download_qr_timer = nullptr;

lv_obj_t *s_wx_city = nullptr;
lv_obj_t *s_wx_date = nullptr;
lv_obj_t *s_wx_temp = nullptr;
lv_obj_t *s_wx_cond = nullptr;
lv_obj_t *s_wx_detail = nullptr;
/* 一屏一天：0 = 现在，1..6 = 服务端 daily[] 里的对应天。 */
lv_obj_t *s_wx_buttons[2] = {};
lv_obj_t *s_wx_next = nullptr;   // “下一天”按钮里的文字
int s_wx_index = 0;
int s_wx_sel = 0;

/* 天气报文先落在缓存里，页面不一定已经建好（服务端通常比用户手快）。 */
struct WeatherDay {
    char day[8] = "";
    char cond[24] = "";
    int tmin = kWeatherNoValue;
    int tmax = kWeatherNoValue;
};

struct WeatherCache {
    bool valid = false;
    int64_t received_at = 0;  // 报文到达时的系统时间，用来推算每页的日期
    char city[32] = "";
    char cond[24] = "";
    int temp = kWeatherNoValue;
    int humidity = kWeatherNoValue;
    int wind = kWeatherNoValue;
    int daily_count = 0;
    WeatherDay daily[kWeatherDayCount];
};

WeatherCache s_weather;

/* ------------------------------------------------------------------ */
/*  小工具                                                             */
/* ------------------------------------------------------------------ */

inline lv_color_t Color(uint32_t hex) { return lv_color_hex(hex); }

int ClampInt(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, Color(color), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    return label;
}

/* 无边框、无内边距、不滚动的容器 —— 圆屏上所有布局都按绝对坐标摆。 */
lv_obj_t *MakeBox(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, Color(kTileColor), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_border_color(box, Color(kAccentColor), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_text_color(box, Color(kTextColor), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t *MakePage(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, kPanelW, kPanelH);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

/* 选中态统一是“3px 强调色描边”，未选中把描边宽度归零。 */
void SetSelected(lv_obj_t *const *objects, int count, int selected) {
    for (int i = 0; i < count; ++i) {
        if (objects[i] != nullptr) {
            lv_obj_set_style_border_width(objects[i], i == selected ? 3 : 0, 0);
        }
    }
}

void ShowOnlyPage(Page page) {
    for (int i = kPageMenu; i < kPageCount; ++i) {
        if (s_pages[i] == nullptr) {
            continue;
        }
        if (i == page) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  主题字体                                                           */
/*                                                                     */
/*  Application::Initialize() 先调 SetupUI()，之后 assets.Apply() 才把  */
/*  字体换成包含中文的 common 字体。所以字体不能在 init 时缓存，每次    */
/*  打开菜单都从当前主题重新取一次（子控件不显式设字体，靠继承）。      */
/* ------------------------------------------------------------------ */

void RefreshFont() {
    if (s_display == nullptr || s_panel == nullptr) {
        return;
    }
    auto *theme = dynamic_cast<LvglTheme *>(s_display->GetTheme());
    if (theme == nullptr || theme->text_font() == nullptr) {
        return;
    }
    const lv_font_t *font = theme->text_font()->font();
    if (font != nullptr) {
        lv_obj_set_style_text_font(s_panel, font, 0);
    }
}

/* ------------------------------------------------------------------ */
/*  菜单页                                                             */
/* ------------------------------------------------------------------ */

/* 图标按钮上的图形全部用 lv_line 画 —— 不依赖主题字体里有没有符号字形，
   也不用缩放图片（放大缩小是逐帧重采样的，圆屏上最费 CPU 的就是它）。 */
void AddCross(lv_obj_t *parent) {
    static lv_point_precise_t diag1[] = {{2, 2}, {20, 20}};
    static lv_point_precise_t diag2[] = {{20, 2}, {2, 20}};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *line = lv_line_create(parent);
        lv_line_set_points(line, i == 0 ? diag1 : diag2, 2);
        lv_obj_set_style_line_width(line, 4, 0);
        lv_obj_set_style_line_color(line, Color(kTextColor), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_size(line, 22, 22);
        lv_obj_center(line);
    }
}

/* 返回箭头 “<” */
void AddChevron(lv_obj_t *parent) {
    static lv_point_precise_t chevron[] = {{15, 2}, {3, 16}, {15, 30}};
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, chevron, 3);
    lv_obj_set_style_line_width(line, 4, 0);
    lv_obj_set_style_line_color(line, Color(kTextColor), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_size(line, 18, 32);
    lv_obj_center(line);
}

/* 二维码磁贴：和 P4 quick_panel 里那个手绘 QR 图元同一套画法，放大到 90px
   与另外三张原生 90x90 图标同尺寸。 */
void AddQrMark(lv_obj_t *parent) {
    lv_obj_t *mark = lv_obj_create(parent);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, 90, 90);
    lv_obj_set_style_bg_color(mark, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark, 8, 0);
    lv_obj_center(mark);

    static const lv_coord_t points[][2] = {
        {36, 9}, {54, 13}, {67, 31}, {36, 45}, {55, 54}, {73, 67},
        {28, 64}, {48, 73}, {70, 48},
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        lv_obj_t *dot = lv_obj_create(mark);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_pos(dot, points[i][0], points[i][1]);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    static const lv_coord_t finder[][2] = {{6, 6}, {56, 6}, {6, 56}};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *outer = lv_obj_create(mark);
        lv_obj_remove_style_all(outer);
        lv_obj_set_size(outer, 28, 28);
        lv_obj_set_pos(outer, finder[i][0], finder[i][1]);
        lv_obj_set_style_border_width(outer, 6, 0);
        lv_obj_set_style_border_color(outer, lv_color_black(), 0);
        lv_obj_set_style_bg_color(outer, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(outer, 0, 0);
    }
}

/* 90x90 图标按原始尺寸摆进 104px 磁贴，不做任何缩放。 */
void AddTileIcon(lv_obj_t *tile, const lv_image_dsc_t *icon) {
    lv_obj_t *image = lv_image_create(tile);
    lv_image_set_src(image, icon);
    lv_obj_center(image);
}

lv_obj_t *MakeTile(lv_obj_t *page, int x, int y) {
    return MakeBox(page, x, y, 104, 104, 22);
}

void BuildMenuPage() {
    lv_obj_t *page = MakePage(s_panel);
    s_pages[kPageMenu] = page;

    s_menu_tiles[0] = MakeTile(page, 69, 50);
    s_menu_tiles[1] = MakeTile(page, 187, 50);
    s_menu_tiles[2] = MakeTile(page, 69, 168);
    s_menu_tiles[3] = MakeTile(page, 187, 168);

    AddTileIcon(s_menu_tiles[0], &xiaowei_asset_icon_sound);
    AddTileIcon(s_menu_tiles[1], &xiaowei_asset_icon_display);
    AddQrMark(s_menu_tiles[2]);
    AddTileIcon(s_menu_tiles[3], &xiaowei_asset_icon_weather);

    /* “关闭”单独一条，确认后收起菜单回到聊天界面。 */
    lv_obj_t *close = MakeBox(page, 105, 280, 150, 42, 21);
    AddCross(close);
    s_menu_tiles[4] = close;

    SetSelected(s_menu_tiles, kMenuTileCount, s_menu_sel);
}

void UpdateMenuSelection() { SetSelected(s_menu_tiles, kMenuTileCount, s_menu_sel); }

/* ------------------------------------------------------------------ */
/*  音量 / 亮度页                                                      */
/* ------------------------------------------------------------------ */

void UpdateAdjustSelection(AdjustPage &ap) {
    SetSelected(ap.buttons, kAdjustButtonCount, ap.sel);
}

/* 标题不用中文，直接用这一页对应的 90x90 图标（原生尺寸，不缩放）。 */
AdjustPage BuildAdjustPage(const lv_image_dsc_t *icon, Page page_id) {
    AdjustPage ap;
    ap.page = MakePage(s_panel);
    s_pages[page_id] = ap.page;

    lv_obj_t *title = lv_image_create(ap.page);
    lv_image_set_src(title, icon);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    ap.value = MakeLabel(ap.page, "0%", &lv_font_montserrat_30, kTextColor);
    lv_obj_align(ap.value, LV_ALIGN_TOP_MID, 0, 114);

    ap.buttons[0] = MakeBox(ap.page, 52, 168, 64, 64, 32);
    lv_obj_t *minus = MakeLabel(ap.buttons[0], "-", &lv_font_montserrat_30, kTextColor);
    lv_obj_center(minus);

    ap.buttons[1] = MakeBox(ap.page, 244, 168, 64, 64, 32);
    lv_obj_t *plus = MakeLabel(ap.buttons[1], "+", &lv_font_montserrat_30, kTextColor);
    lv_obj_center(plus);

    ap.buttons[2] = MakeBox(ap.page, 105, 286, 150, 42, 21);
    AddChevron(ap.buttons[2]);

    SetSelected(ap.buttons, kAdjustButtonCount, ap.sel);
    return ap;
}

void RefreshVolumePage() {
    AudioCodec *codec = Board::GetInstance().GetAudioCodec();
    s_volume.value_now = (codec != nullptr) ? codec->output_volume() : 0;
    lv_label_set_text_fmt(s_volume.value, "%d%%", s_volume.value_now);
    s_volume.sel = 0;
    UpdateAdjustSelection(s_volume);
}

void RefreshBrightnessPage() {
    Backlight *backlight = Board::GetInstance().GetBacklight();
    s_brightness.value_now = (backlight != nullptr) ? backlight->brightness() : 100;
    lv_label_set_text_fmt(s_brightness.value, "%d%%", s_brightness.value_now);
    s_brightness.sel = 0;
    UpdateAdjustSelection(s_brightness);
}

void ChangeVolume(int delta) {
    AudioCodec *codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    s_volume.value_now = ClampInt(s_volume.value_now + delta, 0, 100);
    codec->SetOutputVolume(s_volume.value_now);  // 内部会写 NVS
    lv_label_set_text_fmt(s_volume.value, "%d%%", s_volume.value_now);
}

void ChangeBrightness(int delta) {
    Backlight *backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    s_brightness.value_now = ClampInt(s_brightness.value_now + delta, kBrightnessMin, 100);
    /* permanent=true 每次按键直接写设置：和 MCP 的 self.display.set_brightness
       一致，也避免“渐变还没走完就退出页面导致没保存”的问题。 */
    backlight->SetBrightness(static_cast<uint8_t>(s_brightness.value_now), true);
    lv_label_set_text_fmt(s_brightness.value, "%d%%", s_brightness.value_now);
}

/* ------------------------------------------------------------------ */
/*  二维码页                                                           */
/* ------------------------------------------------------------------ */

/* 和 P4 的 qrcode app 用同一个绑定地址。 */
constexpr const char *kBindUrl = "http://47.108.216.163:8002/?#/home";

void BuildQrPage() {
    lv_obj_t *page = MakePage(s_panel);
    s_pages[kPageQr] = page;

    lv_obj_t *code = lv_qrcode_create(page);
    lv_qrcode_set_size(code, 180);
    lv_qrcode_set_dark_color(code, lv_color_black());
    lv_qrcode_set_light_color(code, lv_color_white());
    lv_qrcode_set_quiet_zone(code, true);
    lv_qrcode_set_data(code, kBindUrl);
    lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 56);

    s_qr_back = MakeBox(page, 105, 286, 150, 42, 21);
    AddChevron(s_qr_back);
    SetSelected(&s_qr_back, 1, 0);
}

/* ------------------------------------------------------------------ */
/*  下载码覆盖层（服务端 artifact_download，自动弹出，不经过按键菜单） */
/* ------------------------------------------------------------------ */

constexpr uint32_t kDownloadQrTimeoutMs = 60000;

void ApplyThemeFontTo(lv_obj_t *obj) {
    if (s_display == nullptr || obj == nullptr) {
        return;
    }
    auto *theme = dynamic_cast<LvglTheme *>(s_display->GetTheme());
    if (theme != nullptr && theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
        lv_obj_set_style_text_font(obj, theme->text_font()->font(), 0);
    }
}

/* 调用方必须已经持有显示锁。 */
void HideDownloadQrLocked() {
    if (s_download_qr_page != nullptr) {
        lv_obj_add_flag(s_download_qr_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_download_qr_timer != nullptr) {
        esp_timer_stop(s_download_qr_timer);
    }
}

void OnDownloadQrTimeout(void *arg) {
    (void)arg;
    if (s_display == nullptr) {
        return;
    }
    DisplayLockGuard lock(s_display);
    HideDownloadQrLocked();
}

void BuildDownloadQrOverlay() {
    /* 直接挂在 lv_layer_top()：盖住聊天界面和按键菜单，不需要用户按任何键。 */
    s_download_qr_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_download_qr_page);
    lv_obj_set_pos(s_download_qr_page, 0, 0);
    lv_obj_set_size(s_download_qr_page, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(s_download_qr_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_download_qr_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_download_qr_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_download_qr_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = MakeLabel(s_download_qr_page, "扫码下载", nullptr, kTextColor);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_download_qr_code = lv_qrcode_create(s_download_qr_page);
    lv_qrcode_set_size(s_download_qr_code, 220);
    lv_qrcode_set_dark_color(s_download_qr_code, lv_color_black());
    lv_qrcode_set_light_color(s_download_qr_code, lv_color_white());
    lv_qrcode_set_quiet_zone(s_download_qr_code, true);
    lv_obj_align(s_download_qr_code, LV_ALIGN_CENTER, 0, 24);

    lv_obj_t *hint = MakeLabel(s_download_qr_page, "用手机扫码保存", nullptr, kMutedColor);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    const esp_timer_create_args_t args = {
        .callback = OnDownloadQrTimeout,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "low_dl_qr",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_download_qr_timer));
}

/* ------------------------------------------------------------------ */
/*  天气页                                                             */
/* ------------------------------------------------------------------ */

/* 可翻的页数：0 号页是"现在"，后面每有一条预报多一页。
 *
 * 服务端 daily[0] 就是今天（和风返回的预报从今天开始），所以 1 号页正好是
 * daily[1]＝明天，依此类推，7 条预报就是"今天 + 未来 6 天"共 7 页。 */
int WeatherPageCount() { return s_weather.daily_count > 1 ? s_weather.daily_count : 1; }

/* 第 index 页的日期文本。
 *
 * 服务端只给了星期缩写（"MON"），没有日历日期；真实日期用开机时 OTA 校过的
 * 系统时间从报文到达时刻往后推。系统时间没校（年月还是 1970）就只显示
 * 今天/明天/后天，再往后退化成星期，什么都没有就写"第 N 天"。 */
void FormatWeatherDate(int index, char *out, size_t out_len) {
    static const char *const kRelative[] = {"今天", "明天", "后天"};
    const char *prefix = index < 3 ? kRelative[index] : nullptr;

    char date[16] = "";
    if (s_weather.received_at > 0) {
        time_t t = (time_t)s_weather.received_at + (time_t)index * 24 * 60 * 60;
        struct tm tm = {};
        if (localtime_r(&t, &tm) != nullptr && tm.tm_year >= 2025 - 1900) {
            std::strftime(date, sizeof(date), "%m-%d", &tm);
        }
    }

    const char *weekday =
        (index < s_weather.daily_count) ? s_weather.daily[index].day : "";

    if (prefix != nullptr) {
        if (date[0] != '\0') {
            std::snprintf(out, out_len, "%s %s", prefix, date);
        } else {
            std::snprintf(out, out_len, "%s", prefix);
        }
    } else if (date[0] != '\0' && weekday[0] != '\0') {
        std::snprintf(out, out_len, "%s %s", date, weekday);
    } else if (date[0] != '\0') {
        std::snprintf(out, out_len, "%s", date);
    } else if (weekday[0] != '\0') {
        std::snprintf(out, out_len, "%s", weekday);
    } else {
        std::snprintf(out, out_len, "第%d天", index + 1);
    }
}

/* 只画当前这一页（每按一次"下一天"就重画一次）：
 *   第 0 页 = 实时：温度、天气、湿度、风
 *   第 1 页起 = 预报：温度区间、天气（服务端 daily 报文里没有湿度和风速，
 *              这两项留空，不编数） */
void ApplyWeatherCache() {
    if (s_wx_city == nullptr) {
        return;
    }

    if (!s_weather.valid) {
        lv_label_set_text(s_wx_city, "暂无天气数据");
        lv_label_set_text(s_wx_date, "");
        lv_label_set_text(s_wx_temp, "");
        lv_label_set_text(s_wx_cond, "");
        lv_label_set_text(s_wx_detail, "");
        lv_label_set_text(s_wx_next, "下一天");
        SetSelected(s_wx_buttons, kWeatherButtonCount, s_wx_sel);
        return;
    }

    const int page_count = WeatherPageCount();
    if (s_wx_index >= page_count) {
        s_wx_index = 0;
    }

    lv_label_set_text(s_wx_city, s_weather.city[0] != '\0' ? s_weather.city : "本地");

    char date[32];
    FormatWeatherDate(s_wx_index, date, sizeof(date));
    lv_label_set_text(s_wx_date, date);

    if (s_wx_index == 0) {
        if (s_weather.temp == kWeatherNoValue) {
            lv_label_set_text(s_wx_temp, "--");
        } else {
            lv_label_set_text_fmt(s_wx_temp, "%d°", s_weather.temp);
        }
        lv_label_set_text(s_wx_cond, s_weather.cond[0] != '\0' ? s_weather.cond : "--");

        char humidity[16];
        if (s_weather.humidity == kWeatherNoValue) {
            std::strncpy(humidity, "--", sizeof(humidity));
        } else {
            std::snprintf(humidity, sizeof(humidity), "%d%%", s_weather.humidity);
        }
        char wind[16];
        if (s_weather.wind == kWeatherNoValue) {
            std::strncpy(wind, "--", sizeof(wind));
        } else {
            std::snprintf(wind, sizeof(wind), "%dkm/h", s_weather.wind);
        }
        lv_label_set_text_fmt(s_wx_detail, "湿度 %s   风 %s", humidity, wind);
    } else {
        const WeatherDay &day = s_weather.daily[s_wx_index];
        if (day.tmin == kWeatherNoValue && day.tmax == kWeatherNoValue) {
            lv_label_set_text(s_wx_temp, "--");
        } else if (day.tmin == kWeatherNoValue) {
            lv_label_set_text_fmt(s_wx_temp, "%d°", day.tmax);
        } else if (day.tmax == kWeatherNoValue) {
            lv_label_set_text_fmt(s_wx_temp, "%d°", day.tmin);
        } else {
            lv_label_set_text_fmt(s_wx_temp, "%d~%d°", day.tmin, day.tmax);
        }
        lv_label_set_text(s_wx_cond, day.cond[0] != '\0' ? day.cond : "--");
        lv_label_set_text(s_wx_detail, "");
    }

    lv_label_set_text(s_wx_next, "下一天");
    SetSelected(s_wx_buttons, kWeatherButtonCount, s_wx_sel);
}

void BuildWeatherPage() {
    lv_obj_t *page = MakePage(s_panel);
    s_pages[kPageWeather] = page;

    s_wx_city = MakeLabel(page, "暂无天气数据", nullptr, kTextColor);
    lv_obj_align(s_wx_city, LV_ALIGN_TOP_MID, 0, 22);

    s_wx_date = MakeLabel(page, "", nullptr, kMutedColor);
    lv_obj_align(s_wx_date, LV_ALIGN_TOP_MID, 0, 50);

    s_wx_temp = MakeLabel(page, "--", &lv_font_montserrat_46, kTextColor);
    lv_obj_align(s_wx_temp, LV_ALIGN_TOP_MID, 0, 74);

    s_wx_cond = MakeLabel(page, "--", nullptr, kTextColor);
    lv_obj_align(s_wx_cond, LV_ALIGN_TOP_MID, 0, 132);

    s_wx_detail = MakeLabel(page, "", nullptr, kMutedColor);
    lv_obj_align(s_wx_detail, LV_ALIGN_TOP_MID, 0, 164);

    /* “下一天”：BOOT 选中、QON 确认。翻到最后一页后再按回到今天，循环。 */
    s_wx_buttons[0] = MakeBox(page, 105, 214, 150, 42, 21);
    s_wx_next = MakeLabel(s_wx_buttons[0], "下一天", nullptr, kTextColor);
    lv_obj_center(s_wx_next);

    s_wx_buttons[1] = MakeBox(page, 105, 286, 150, 42, 21);
    AddChevron(s_wx_buttons[1]);

    s_wx_sel = 0;
    SetSelected(s_wx_buttons, kWeatherButtonCount, s_wx_sel);
}

/* ------------------------------------------------------------------ */
/*  页面切换 / 面板开合                                                */
/* ------------------------------------------------------------------ */

void EnterPage(Page page) {
    switch (page) {
        case kPageVolume:
            RefreshVolumePage();
            break;
        case kPageBrightness:
            RefreshBrightnessPage();
            break;
        case kPageWeather:
            /* 每次从菜单进来都从“今天”开始看；页面内翻页不清零。 */
            s_wx_index = 0;
            s_wx_sel = 0;
            ApplyWeatherCache();
            break;
        case kPageMenu:
            UpdateMenuSelection();
            break;
        default:
            break;
    }
    s_page = page;
    ShowOnlyPage(page);
}

/* 调用方必须已经持有显示锁。 */
void ClosePanelLocked() {
    if (s_panel == nullptr) {
        return;
    }
    lv_anim_delete(s_panel, nullptr);
    lv_obj_set_y(s_panel, -kPanelH);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_page = kPageClosed;
}

/* 调用方必须已经持有显示锁。从下滑入指定页面。 */
void OpenPanelLocked(Page page) {
    if (s_panel == nullptr) {
        return;
    }
    /* 记下呼出时的状态：唤醒后再按 BOOT 也必须能进菜单。 */
    s_last_state = Application::GetInstance().GetDeviceState();

    RefreshFont();
    EnterPage(page);

    lv_anim_delete(s_panel, nullptr);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_panel, -kPanelH);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_exec_cb(&anim, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y));
    lv_anim_set_values(&anim, -kPanelH, 0);
    lv_anim_set_duration(&anim, kSlideMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

/* 语音/配网/升级等状态出现时自动收起，避免菜单盖住对话界面。
   定时器跑在 LVGL 任务里（esp_lvgl_port 已经在 lv_timer_handler 外面
   上锁），这里再取一次锁是可重入的。 */
bool IsVoiceState(DeviceState state) {
    switch (state) {
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateUpgrading:
            return true;
        default:
            return false;
    }
}

void AutoCloseTimer(lv_timer_t *timer) {
    (void)timer;
    if (s_page == kPageClosed || s_display == nullptr) {
        return;
    }

    DeviceState state = Application::GetInstance().GetDeviceState();
    bool jumped_into_voice = IsVoiceState(state) && !IsVoiceState(s_last_state);
    s_last_state = state;

    if (!jumped_into_voice) {
        return;
    }

    DisplayLockGuard lock(s_display);
    if (s_page != kPageClosed) {
        ClosePanelLocked();
    }
}

/* ------------------------------------------------------------------ */
/*  天气报文解析                                                       */
/* ------------------------------------------------------------------ */

void JsonText(const cJSON *obj, const char *key, char *out, size_t out_len) {
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (obj == nullptr) {
        return;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return;
    }
    std::strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
}

int JsonIntOrNa(const cJSON *obj, const char *key) {
    if (obj == nullptr) {
        return kWeatherNoValue;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : kWeatherNoValue;
}

}  // namespace

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

void low_ui_init(Display *display) {
    if (display == nullptr || s_panel != nullptr) {
        return;
    }
    s_display = display;

    DisplayLockGuard lock(display);

    /* 挂在顶层：不属于任何 screen，聊天界面完全不用改。 */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, kPanelW, kPanelH);
    lv_obj_set_pos(s_panel, 0, -kPanelH);
    lv_obj_set_style_bg_color(s_panel, Color(kBgColor), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_text_color(s_panel, Color(kTextColor), 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    RefreshFont();

    BuildMenuPage();
    s_volume = BuildAdjustPage(&xiaowei_asset_icon_sound, kPageVolume);
    s_brightness = BuildAdjustPage(&xiaowei_asset_icon_display, kPageBrightness);
    BuildQrPage();
    BuildWeatherPage();
    BuildDownloadQrOverlay();

    ShowOnlyPage(kPageMenu);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_page = kPageClosed;

    s_auto_close_timer = lv_timer_create(AutoCloseTimer, 500, nullptr);
    (void)s_auto_close_timer;

    ESP_LOGI(TAG, "Low UI menu ready");
}

void low_ui_open_menu() {
    if (s_panel == nullptr || s_display == nullptr) {
        return;
    }

    DisplayLockGuard lock(s_display);
    HideDownloadQrLocked();
    OpenPanelLocked(kPageMenu);
}

bool low_ui_is_open() { return s_page != kPageClosed; }

bool low_ui_on_select() {
    if (s_panel == nullptr || s_display == nullptr) {
        return false;
    }

    DisplayLockGuard lock(s_display);

    switch (s_page) {
        case kPageClosed:
            return false;
        case kPageMenu:
            s_menu_sel = (s_menu_sel + 1) % kMenuTileCount;
            UpdateMenuSelection();
            break;
        case kPageVolume:
            s_volume.sel = (s_volume.sel + 1) % kAdjustButtonCount;
            UpdateAdjustSelection(s_volume);
            break;
        case kPageBrightness:
            s_brightness.sel = (s_brightness.sel + 1) % kAdjustButtonCount;
            UpdateAdjustSelection(s_brightness);
            break;
        case kPageQr:
            /* 只有“返回”一项，再按 BOOT 没有别的东西可选。 */
            break;
        case kPageWeather:
            /* 在“下一天”和“返回”之间循环。 */
            s_wx_sel = (s_wx_sel + 1) % kWeatherButtonCount;
            SetSelected(s_wx_buttons, kWeatherButtonCount, s_wx_sel);
            break;
        default:
            break;
    }
    return true;
}

void low_ui_on_confirm() {
    if (s_panel == nullptr || s_display == nullptr) {
        return;
    }

    DisplayLockGuard lock(s_display);

    switch (s_page) {
        case kPageClosed:
            return;

        case kPageMenu:
            switch (s_menu_sel) {
                case 0:
                    EnterPage(kPageVolume);
                    break;
                case 1:
                    EnterPage(kPageBrightness);
                    break;
                case 2:
                    EnterPage(kPageQr);
                    break;
                case 3:
                    EnterPage(kPageWeather);
                    break;
                case 4:
                    ClosePanelLocked();
                    break;
                default:
                    break;
            }
            break;

        case kPageVolume:
            if (s_volume.sel == 0) {
                ChangeVolume(-10);
            } else if (s_volume.sel == 1) {
                ChangeVolume(10);
            } else {
                EnterPage(kPageMenu);
            }
            break;

        case kPageBrightness:
            if (s_brightness.sel == 0) {
                ChangeBrightness(-10);
            } else if (s_brightness.sel == 1) {
                ChangeBrightness(10);
            } else {
                EnterPage(kPageMenu);
            }
            break;

        case kPageQr:
            EnterPage(kPageMenu);
            break;

        case kPageWeather:
            if (s_wx_sel == 0) {
                const int count = WeatherPageCount();
                if (count > 1) {
                    s_wx_index = (s_wx_index + 1) % count;
                    ApplyWeatherCache();
                }
            } else {
                EnterPage(kPageMenu);
            }
            break;

        default:
            break;
    }
}

void low_ui_set_weather(const char *json, size_t len) {
    if (json == nullptr || len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to parse weather payload (%u bytes)", static_cast<unsigned>(len));
        return;
    }

    WeatherCache cache;
    cache.valid = true;
    cache.received_at = (int64_t)time(nullptr);
    JsonText(root, "city", cache.city, sizeof(cache.city));

    const cJSON *now = cJSON_GetObjectItem(root, "now");
    cache.temp = JsonIntOrNa(now, "temp");
    cache.humidity = JsonIntOrNa(now, "humidity");
    cache.wind = JsonIntOrNa(now, "wind");
    JsonText(now, "cond", cache.cond, sizeof(cache.cond));

    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const int daily_count = cJSON_IsArray(daily) ? cJSON_GetArraySize(daily) : 0;
    for (int i = 0; i < daily_count && cache.daily_count < kWeatherDayCount; ++i) {
        const cJSON *day = cJSON_GetArrayItem(daily, i);
        WeatherDay *out = &cache.daily[cache.daily_count];
        JsonText(day, "day", out->day, sizeof(out->day));
        JsonText(day, "cond", out->cond, sizeof(out->cond));
        out->tmin = JsonIntOrNa(day, "tmin");
        out->tmax = JsonIntOrNa(day, "tmax");
        ++cache.daily_count;
    }

    cJSON_Delete(root);
    s_weather = cache;

    ESP_LOGI(TAG, "Weather received: %s %s %d, %d days", s_weather.city, s_weather.cond,
             s_weather.temp, s_weather.daily_count);

    if (s_panel == nullptr || s_display == nullptr) {
        return;  // 菜单还没建好，数据留在缓存里
    }

    DisplayLockGuard lock(s_display);
    if (s_page == kPageWeather) {
        ApplyWeatherCache();
    }
}

bool low_ui_show_download_qr(const char *url, size_t len) {
    if (url == nullptr || len == 0 || len > 512 ||
        (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0)) {
        return false;
    }
    if (s_display == nullptr || s_download_qr_page == nullptr || s_download_qr_code == nullptr) {
        return false;
    }

    DisplayLockGuard lock(s_display);
    ApplyThemeFontTo(s_download_qr_page);
    /* lv_qrcode_update() 内部会拷贝 data，这里用完即可释放。 */
    const std::string text(url, len);
    lv_qrcode_set_data(s_download_qr_code, text.c_str());
    lv_obj_move_foreground(s_download_qr_page);
    lv_obj_remove_flag(s_download_qr_page, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(s_download_qr_timer);
    esp_timer_start_once(s_download_qr_timer, (uint64_t)kDownloadQrTimeoutMs * 1000);
    ESP_LOGI(TAG, "Download QR shown (%u bytes)", static_cast<unsigned>(len));
    return true;
}

void low_ui_hide_download_qr() {
    if (s_display == nullptr) {
        return;
    }
    DisplayLockGuard lock(s_display);
    HideDownloadQrLocked();
}
