#ifndef C61_MODULE_H
#define C61_MODULE_H

#include <esp_err.h>

/**
 * @brief ESP32-C61 无线协处理器驱动（U14: ESP32-C61-MINI-1/U）
 *
 * 本板 ESP32-P4 自身没有无线能力，WiFi 由板载的这颗 C61 提供。
 * 本驱动只负责"把 C61 拉起来并确认它在跑"这一段：复位 + UART 探针。
 * 真正的联网（SDIO + esp-hosted）由 esp_hosted / esp_wifi_remote 两个组件在
 * 后台完成，板级代码不需要写 —— 这正是官方 P4 板（esp32-p4-function-ev-board）
 * 的做法：板级 .cc 里一个 esp_hosted 调用都没有，全靠 Kconfig 自启。
 *
 * 不属于板级类：本类不继承 WifiBoard，也不实现任何外设接口。
 * 一个板级目录只允许有一个 `DECLARE_BOARD`（官方规范），那个名额归
 * xiaowei_plus.cc 的 XiaoweiPlusBoard，本文件是被它持有的一个驱动。
 *
 * 错误处理约定：所有方法返回 esp_err_t，失败只打日志不抛异常、不 panic。
 * 绝不用 ESP_ERROR_CHECK 包裹 —— C61 没起来只应该意味着"无线不可用"，
 * 不该拖垮整台设备。
 */
class C61Module {
public:
    C61Module();
    ~C61Module();

    /**
     * @brief 完整初始化：装串口 -> 复位 -> 抓启动日志
     * 幂等性不做要求，正常路径只调用一次。任何一步失败都只记日志，
     * 返回值只用于判断"探针流程有没有跑完"，不代表 C61 一定活着（看 IsOnline()）。
     */
    esp_err_t Init();

    /**
     * @brief 复位 C61 并把它的 UART 输出逐行打到日志里
     * 非可打印字符用 '.' 代替。用于判断模块里到底有没有固件、有没有跑起来。
     * @param duration_ms 抓取窗口，单位毫秒
     * @return ESP_OK 表示探针流程跑完
     */
    esp_err_t Probe(int duration_ms);

    /** @brief 探针窗口内是否收到过字节（C61 是否活着） */
    bool IsOnline() const {
        return online_;
    }

    /** @brief UART 是否已就绪 */
    bool IsUartReady() const {
        return uart_ready_;
    }

private:
    /**
     * @brief 安装并配置 C61 串口（UART1）
     * 幂等：已就绪时直接返回 ESP_OK。
     * @return ESP_OK 表示串口可用
     */
    esp_err_t EnsureUart();

    /**
     * @brief EN 拉低再放开，硬复位 C61
     * EN 用 GPIO_MODE_INPUT_OUTPUT 配置，复位的同时回读真实电平 ——
     * 外部若是漏焊/短接把 EN 拉住了，日志里能直接看出来。
     * @return ESP_OK 表示复位脉冲已发出
     */
    esp_err_t Reset();

    /**
     * @brief 把 C61 的 IO9（下载模式 strapping）置为高，让它跑 flash 里的固件
     * 本板 IO9 未引出，默认是空实现；只有飞线接到 DEBUG_TP5_PIN 时才生效。
     */
    void HoldIo9ForBoot();

    // ===== C61 无线协处理器（U14: ESP32-C61-MINI-1/U）=====
    bool uart_ready_ = false;   // UART1 驱动是否已安装
    bool online_ = false;       // 探针结果：C61 是否吐出过启动日志
};

#endif  // C61_MODULE_H
