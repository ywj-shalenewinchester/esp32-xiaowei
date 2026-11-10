// ============================================================================
//  CO5300 466x466 AMOLED（J4，MIPI-DSI 单 lane）+ 亮度控制
//
//  序列搬自板级参考实现 plus/lcd 的 main/main.cpp —— 那一份是在这块硬件上真跑过的。
//  ⚠ 别参考 plus/lcd/main/boards/WZKJ/.../xiaowei_plus.cc：那个文件根本没进编译
//    （不在它家 CMakeLists 的 SOURCES 里），而且里面有三处是错的 —— 写成 2 lane、
//    漏了 DSI PHY 的 LDO、还错继承了 SpiLcdDisplay（MIPI 屏得用 MipiLcdDisplay）。
// ============================================================================

#pragma once

#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>

#include "backlight.h"

class Lcd {
public:
    Lcd() = default;
    ~Lcd();

    // 持有 LDO / DSI 总线等独占硬件资源，拷贝没有意义
    Lcd(const Lcd&) = delete;
    Lcd& operator=(const Lcd&) = delete;

    // DSI PHY LDO -> DSI 总线 -> DBI IO -> CO5300 面板 -> 复位/初始化/唤醒。
    // 逐失败返回错误码而不是 ESP_ERROR_CHECK：屏没起来时板级要能退回无 UI 后端，
    // 让网络和音频先跑起来（整机固件不能因为一块屏起不来就死在启动阶段）。
    esp_err_t Init();

    // 只有 Init() 整条跑通才是 true（句柄建出来≠面板能用：复位/初始化失败时
    // 句柄是存在的，面板却是死的）。
    bool Ready() const { return ready_; }

    // 交给 MipiLcdDisplay(panel_io, panel, ...) 用
    esp_lcd_panel_io_handle_t panel_io() const { return panel_io_; }
    esp_lcd_panel_handle_t panel() const { return panel_; }

    // ---- 句柄所有权（很重要，别省）----
    // MipiLcdDisplay 的析构函数会 esp_lcd_panel_del + esp_lcd_panel_io_del 我们传进去的
    // 这两个句柄（见 display/lcd_display.cc 的 ~LcdDisplay）。所以：
    //   HandOffPanel()  建好显示对象后立刻调 —— 本类从此不再删面板/IO（交给显示对象），
    //                   panel()/panel_io() 仍可继续用（那个对象还活着）。
    //   不调就 double free：两边的析构都会去删同一对句柄。
    void HandOffPanel() { owns_panel_ = false; }

    // 显示对象**即将被删除**时调：面板句柄马上失效，把引用清干净。
    // 这一步是为了堵住一个使用后释放窗口 —— 亮度渐变的定时器要等到 backlight_
    // 成员析构（在 delete display_ 之后）才会停，中间那一小段里 SetBrightness()
    // 会带着一个已经释放的 panel 指针进来。
    void ForgetPanel() {
        panel_ = nullptr;
        panel_io_ = nullptr;
        ready_ = false;
    }

    // 0-100（0 = 全黑）。写的是面板的 0x51 亮度寄存器，不是 PWM 背光。
    esp_err_t SetBrightness(uint8_t percent);

private:
    esp_ldo_channel_handle_t dsi_phy_ldo_ = nullptr;
    esp_lcd_dsi_bus_handle_t dsi_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool owns_panel_ = false;  // 只有 Init() 成功到 HandOffPanel() 之间为 true
    bool ready_ = false;       // Init() 整条跑通才 true（HandOffPanel 不动它）
};

// ============================================================================
//  把面板亮度接到框架的 Backlight 抽象上
//
//  本板没有独立背光脚（config.h 里 DISPLAY_BACKLIGHT_PIN = GPIO_NUM_NC），
//  GetBacklight() 以前返回 nullptr，于是框架和 MCP 的亮度功能整条是哑的。
//  Backlight 基类负责持久化(Settings)、渐变定时器和 RestoreBrightness()，
//  这里只需要把 0-100 的亮度值转给面板。
// ============================================================================
class Co5300Backlight : public Backlight {
public:
    explicit Co5300Backlight(Lcd* lcd) : lcd_(lcd) {}

    void SetBrightnessImpl(uint8_t brightness) override;

private:
    Lcd* lcd_;
};
