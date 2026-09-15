# WZKJ ESP32-S3 AI Xiaowei Low（自定义低功耗版）

基于 ESP32-S3 的圆屏语音助手板，360×360 QSPI 圆形 LCD + ES8311 音频编解码 + SY6206 电源管理。

## 硬件规格

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-S3 |
| Flash | 16MB |
| PSRAM | 8MB Octal SPI（`CONFIG_SPIRAM_MODE_OCT=y`） |
| 屏幕 | H0153Y002 V1 / ST77916，360×360 圆屏，QSPI 接口 |
| 音频编解码 | ES8311（I2C 地址 0x30） |
| 功放 | NS4150B |
| 电源管理 | SY6206（I2C 地址 0x6B） |
| 分区表 | `partitions/v2/16m.csv`（工程默认值） |

## 引脚分配

完整定义见 [config.h](config.h)。

### 屏幕（QSPI，SPI2_HOST）

| 信号 | GPIO |
| --- | --- |
| QSPI_CLK | 5 |
| QSPI_D0 | 16 |
| QSPI_D1 | 15 |
| QSPI_D2 | 7 |
| QSPI_D3 | 6 |
| LCD_CS | 17 |
| LCD_RST | 4 |
| LCD_BL（背光 PWM） | 3 |

### 音频（ES8311，I2C_NUM_1）

| 信号 | GPIO |
| --- | --- |
| I2C SDA | 12 |
| I2C SCL | 11 |
| MCLK | 10 |
| SCLK | 18 |
| LRCK | 19 |
| DAC_DATA（主控 → 编解码） | 9 |
| ADC_DATA（编解码 → 主控） | 8 |
| PA_CTRL（NS4150B 使能） | 48 |

采样率：输入 / 输出均为 24000Hz。

### 电源管理（SY6206，I2C_NUM_0）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| PMIC I2C SDA | 39 | |
| PMIC I2C SCL | 40 | |
| CE | 38 | 充电使能，低有效 |
| /PG | 14 | 电源状态，低 = 正常 |
| STAT | 47 | 充电状态，低 = 充电中 |
| INT | 41 | 中断，低有效 |
| QON | 21 | 电源按键 / 深度睡眠唤醒源 |

### 按键与 LED

| 名称 | GPIO | 功能 |
| --- | --- | --- |
| BOOT | 0 | 短按唤醒 / 切换对话（空闲时开始对话，回复中则打断）；启动期短按进入配网 |
| QON | 21 | 长按关机（进入深度睡眠），再按 QON 唤醒 |
| R_LED | 13 | 红色指示灯 |

## 板级实现说明

- **显示**：`WzkjLcdDisplay` 继承 `SpiLcdDisplay`，复用框架 UI（状态栏、聊天气泡、表情、配网提示、主题切换），仅按圆屏安全区调整了状态栏与字幕条的位置和宽度。
- **背光**：使用框架的 `PwmBacklight`，因此 MCP 的 `self.screen.set_brightness`、设备状态上报的 `brightness` 以及低功耗熄屏都可用。
- **电池**：`GetBatteryLevel()` 通过 SY6206 读取充电状态与 VBAT ADC，按 3.3V–4.2V 线性映射到 0–100%，充电中未充满时上限锁定 99%。
- **关机**：QON 长按经 `Application::Schedule` 在主任务执行，关闭背光与显示面板后进入深度睡眠，并对 QON 使能 RTC 上拉以避免悬空误唤醒。
- **MCP 工具**：`self.disp.network` 可让 AI 触发重新配网。

## 编译配置命令

**配置编译目标为 ESP32S3：**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```
Xiaozhi Assistant -> Target Board -> WZKJ ESP32-S3 AI Xiaowei Low (自定义低功耗版)
```

**编译：**

```bash
python ./scripts/build.py wzkj/esp32-s3-ai-xiaowei_low
```

**下载并打开串口终端：**

```bash
idf.py flash monitor
```

> 分区的 / Flash 容量使用工程默认值（16MB Flash + `partitions/v2/16m.csv`），
> 如需修改请通过修改板子的 `config.json` 中的 `sdkconfig_append` 覆盖。
