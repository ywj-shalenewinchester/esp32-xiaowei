// ============================================================================
//  CST820B 电容触摸（挂在 J4 那路 I2C 上，和摄像头共用）
//
//  协议搬自板级参考实现 plus/lcd/main/boards/WZKJ/.../lcd.cc 的 lcd_touch_init /
//  lcd_touch_read（那一份在实机上验证过）。
//  ⚠ 那边的手势识别（单击/双击/长按/滑动那个 gesture_task）**不搬** —— 主工程里
//    没有消费方，搬过来只是白跑一个任务。
//
//  两个已知的"故意不做"：
//    - TOUCH_INT_PIN(GPIO19) 没接：读触摸走 LVGL 的轮询回调（每帧读一次），
//      不做中断驱动 —— 和参考工程一致。
//    - I2C_NUM_1 由本类自己建（板上只有触摸在用）。将来 DVP 摄像头落地时它也要挂
//      这条总线，到时候要么把建总线提到板级，要么给 Init() 加个传入总线的重载。
// ============================================================================

#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <lvgl.h>

class LcdTouch {
public:
    LcdTouch() = default;
    ~LcdTouch();

    LcdTouch(const LcdTouch&) = delete;
    LcdTouch& operator=(const LcdTouch&) = delete;

    // 复位触摸芯片（低 5ms -> 高 20ms），然后自建 I2C 总线并挂上 0x15。
    // 这条总线（I2C_NUM_1）本板只有触摸在用，所以自己建、自己管。
    esp_err_t Init();

    // 读一次触点。返回 false = I2C 读失败（*x/*y/*pressed 不动）；
    // 返回 true 时 *pressed 为 false 表示当前没按（此时 *x/*y 也不更新）。
    bool Read(uint16_t* x, uint16_t* y, bool* pressed);

    // 注册成 LVGL 指针输入设备。必须在 MipiLcdDisplay 建好之后调
    // （那时 LVGL 的默认 display 才存在）。disp 传 nullptr 则取默认 display。
    esp_err_t AttachToLvgl(lv_display_t* disp);

private:
    static void ReadCb(lv_indev_t* indev, lv_indev_data_t* data);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    lv_indev_t* indev_ = nullptr;
    bool reading_failed_ = false;  // 只在读失败的状态发生翻转时记一条日志，别刷屏

    // 读失败时"当作没采样"要用的上一次状态，理由见 touch.cc 的 ReadCb。
    // 关键点：失败**绝不能**报"抬手"——那会把一次按着不放的拖拽判成松开，
    // 于是补发 CLICKED（拖拽途中凭空点开下一级页面）并重新武装手势
    // （手指每再走 50px 再翻一页）。last_point_ 也不能省：报 PRESSED 却把坐标
    // 留在 (0,0)，下一帧 vect 就是一整屏的跳变，照样立刻触发一次手势。
    lv_indev_state_t last_state_ = LV_INDEV_STATE_RELEASED;
    lv_point_t last_point_ = {0, 0};
    uint32_t fail_since_ms_ = 0;  // 连续读失败的起点；0 = 当前没有在失败

    // 读到"没按"的起点（仅在上一帧还按着时才有意义）；0 = 当前没有在等确认。
    // 见 touch.cc 的 TOUCH_RELEASE_DEBOUNCE_MS：读**成功**但报没按，也要先确认
    // 一下再认账 —— 单帧假抬手在 LVGL 那边就是一次 CLICKED。
    uint32_t release_since_ms_ = 0;
};

extern "C" i2c_master_bus_handle_t xiaowei_camera_i2c_bus(void);
