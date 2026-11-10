// 低配版（ESP32-S3，360x360 圆屏，无触摸）按键快捷菜单
//
// 交互约定（两个物理按键，没有触摸）：
//   BOOT 短按 —— 菜单没开时呼出下滑菜单；菜单开着时循环选择
//   QON  短按 —— 确认当前选中项
//   BOOT 长按 —— 原来的“唤醒/开始或打断对话”（在 board 的按键回调里处理）
//   QON  长按 —— 关机（在 board 的按键回调里处理）
//
// 首页保持 LcdDisplay::SetupUI() 建出来的聊天界面不动；菜单挂在
// lv_layer_top() 上，收起后原来的界面原样可见。

#ifndef LOW_UI_H
#define LOW_UI_H

#include <cstddef>

class Display;

// 在显示初始化好之后调用一次（WzkjLcdDisplay::SetupUI 里）。
void low_ui_init(Display* display);

// 滑下菜单（默认选中“音量”）。
void low_ui_open_menu();

// 菜单是否处于展开状态（任意子页也算展开）。
bool low_ui_is_open();

// BOOT 短按。返回 true 表示这次按键被菜单消费了；
// 返回 false 表示菜单没开，调用方应该执行“呼出菜单”。
bool low_ui_on_select();

// QON 短按。菜单没开时什么也不做。
void low_ui_on_confirm();

// 服务端下发的天气 JSON（application.cc 里 type=="weather" 的报文）。
// 天气页还没建也不会丢数据，打开页面时用缓存渲染。
void low_ui_set_weather(const char* json, size_t len);

// 服务端下发的下载二维码（artifact_download 报文）。
// 在 lv_layer_top() 上弹出整屏二维码覆盖层；url 必须是 http(s)，
// 最长 512 字节。60 秒后自动收起。成功返回 true。
bool low_ui_show_download_qr(const char* url, size_t len);

// 收起下载二维码（新任务/新图片开始时调用）。
void low_ui_hide_download_qr();

#endif  // LOW_UI_H
