# WZKJ ESP32-P4 AI Xiaowei Plus（小未 Plus）

基于 **ESP32-P4 + ESP32-C61** 的语音助手板。P4 承担主控与算力，自身没有无线能力，
WiFi/蓝牙由板载的 **ESP32-C61-MINI-1（U14）** 无线协处理器经 SDIO 通过 `esp_hosted` 提供。

## 硬件规格

| 项目 | 规格 |
| --- | --- |
| 主控 | ESP32-P4 |
| 无线 | ESP32-C61-MINI-1（U14），SDIO 4-bit 接 P4，`esp_hosted` + `esp_wifi_remote` |
| Flash | 16MB QSPI NOR（U16: W25Q12RVCPJQ），`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`，分区表 `partitions/16m.csv` |
| PSRAM | 板载，HEX 模式 200MHz（`SPIRAM_MODE_HEX` + `SPIRAM_SPEED_200M`） |
| 显示 | CO5300 466x466 圆屏 AMOLED，MIPI DSI 单 lane（J4）；控制脚 RST/TE 已引出 |
| 触摸 | CST820B（I2C 0x15），挂在 J4，与摄像头共享总线 |
| 摄像头 | DVP 8-bit（J3），数据经 RN1/RN2/RN3 |
| 音频 | 数字功放 U8: AW88082QNR（I2S）+ PDM 数字麦 U7 |
| 电源管理 | SY6206（I2C 0x6B，外设总线） |
| 输入保护 | SY5321（~EN 使能 + ~FLT 故障监测） |
| IMU | LSM6DSV80X（I2C 0x6A，外设总线） |
| 振动马达 | AW86937（I2C 0x5A，外设总线），波形经 RAM 下载 |

## 引脚分配

完整定义见 [config.h](config.h)。以下按该文件的 8 个小节列出。

### 1. 系统、电源与控制

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| PMIC_INT | 16 | SY6206 中断输入（低有效，读状态寄存器清中断） |
| PMIC_STAT | 17 | 充电状态（0 = 充电中；**不能单独作充满判据**） |
| PMIC_QON | 18 | ~QON / SW1 电源键，由 PMIC 硬件处理（见下文「QON 为什么没接管」） |
| POWER_PROT_EN | 5 | SY5321 输入保护使能，低有效 |
| POWER_PROT_FLT | 6 | SY5321 输入故障，低有效 |
| BOOT | 35 | BOOT 按钮 / TP4 |
| DEBUG_TP5 | 4 | 调试测试点 TP5（也是 C61 IO9 的飞线点） |

`CHIP_PU` / `EN_DCDC` / `USB_JTAG_D±` 由硬件或专用外设接管，没有标准 GPIO 编号。

### 2. I2C 总线分配

| 总线 | SDA | SCL | 端口 | 挂载 |
| --- | --- | --- | --- | --- |
| 外设总线 | 39 | 40 | `I2C_NUM_0` | SY6206、LSM6DSV80X、AW88082QNR、AW86937（经 RN6 上拉到 +1V8） |
| 摄像头/触摸总线 | 50 | 49 | `I2C_NUM_1` | J3 DVP 摄像头、J4 DSI 触摸 |

全板两条总线，板级类创建**外设**这一条 + 触摸那条（`I2C_NUM_1`，只挂 CST820B；
摄像头驱动还没落地，将来要共用这条总线的话直接在 `LcdTouch` 建的总线上挂设备即可）。

### 3. 显示与触摸（MIPI DSI）

| 信号 | GPIO |
| --- | --- |
| DSI_RESET | 21 |
| DSI_TE | 22 |
| DSI_TP_INT | 19 |
| DSI_TP_RST | 20 |

MIPI 数据/时钟走专用 PHY 引脚（J4.23/24、26/27、29/30），**没有标准 GPIO 编号**。

- **面板**：CO5300，466x466 圆屏 AMOLED，**单 lane** MIPI-DSI @480Mbps，
  DPI 时钟 16MHz（配 32/4/32 + 12/4/18 前后肩 = 60Hz），RGB565，帧缓冲 1 个 + DMA2D。
  DSI PHY 的供电来自芯片内部 LDO 通道 3 @2.5V，**必须先于 DSI 主机建立**。
  驱动在 `lcd.h/lcd.cc`（类 `Lcd`），`esp_lcd_co5300` 依赖按 target 隔离在
  `main/idf_component.yml` 里。
- **触摸**：CST820B，I2C 地址 `0x15` @400kHz（就是上表的摄像头/触摸总线）。
  驱动在 `touch.h/touch.cc`（类 `LcdTouch`），复位脚 TP_RST(GPIO20)。
  它被注册成 LVGL 的**指针输入设备**，坐标 466x466 原样上报（1:1 不缩放）；
  参考工程里的手势识别（单击/双击/长按/滑动）没有移植 —— 主工程里没有消费方。
  TP_INT(GPIO19) **没接**：读触摸走 LVGL 的轮询回调（每帧读一次 `0x02` 起 6 字节），
  不做中断驱动 —— 和参考工程一致。
- **亮度**：本板**没有独立背光脚**，亮度是面板自己的 `0x51` 寄存器
  （`Lcd::SetBrightness()` / `esp_lcd_panel_co5300_set_brightness`，0 = 全黑），
  由 `Co5300Backlight : public Backlight` 接到框架的 `GetBacklight()` 上，
  于是 `self.screen.set_brightness` 和亮度持久化都能用。
- ⚠ 不要对面板调 `esp_lcd_panel_mirror/swap_xy`：CO5300 的 MIPI 驱动没实现这两个，
  真要转屏得走面板的 MADCTL 命令。
- ⚠ **面板句柄的所有权**：`~LcdDisplay()`（`display/lcd_display.cc`）会
  `esp_lcd_panel_del` + `esp_lcd_panel_io_del` 构造时传进去的句柄。所以
  `InitializeLcd()` 在 `new MipiLcdDisplay(...)` 之后**必须**调 `lcd_.HandOffPanel()`，
  析构时**必须先** `lcd_.ForgetPanel()` 再 `delete display_` —— 少一步就是 double free
  或者亮度渐变定时器写已释放内存。细节见 `lcd.h`。

### 4. 摄像头（DVP）

| 信号 | GPIO | | 信号 | GPIO |
| --- | --- | --- | --- | --- |
| CAM_D0 | 29 | | CAM_PCLK | 30 |
| CAM_D1 | 28 | | CAM_HSYNC | 34 |
| CAM_D2 | 27 | | CAM_VSYNC | 36 |
| CAM_D3 | 26 | | CAM_XCLK | 38 |
| CAM_D4 | 33 | | CAM_PWDN | 52 |
| CAM_D5 | 32 | | CAM_RST | 51 |
| CAM_D6 | 31 | | | |
| CAM_D7 | 37 | | | |

### 5. 音频

采样率输入/输出均为 **24000Hz**。

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| AUDIO_MIC_DATA | 47 | PDM 麦克风数据（U7） |
| AUDIO_MIC_CLK | 48 | PDM 麦克风时钟 |
| AUDIO_AMP_WCK | 41 | I2S WS / LRCK |
| AUDIO_AMP_BCK | 43 | I2S BCK |
| AUDIO_AMP_DATAI | 42 | I2S DATA_IN |
| AUDIO_AMP_DATAO | 44 | I2S DATA_OUT |

这是 **I2S 数字功放 + PDM 数字麦**，不是 I2C 编解码器，框架现成的
`Es8311AudioCodec` 不适用。

### 6. 振动马达（AW86937）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| HAPTIC_RST | 0 | RSTN 复位控制。**注意：芯片处于 RSTN 复位态时 I2C 不响应**，所以总线扫描扫不到 0x5A 是正常的 |
| HAPTIC_TRIG1/2/3 | 3 / 2 / 1 | 触发脉冲 |

### 7. 无线协处理器（ESP32-C61-MINI-1）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| WIFI_EN | 7 | C61 的 EN，**低 = 复位**，高 = 运行 |
| WIFI_UART_TX | 8 | C61 的 **TX0** |
| WIFI_UART_RX | 9 | C61 的 **RX0** |
| WIFI_SDIO_D3 / D2 / D1 / D0 | 10 / 11 / 12 / 13 | SDIO 数据 |
| WIFI_SDIO_CLK | 14 | SDIO 时钟 |
| WIFI_SDIO_CMD | 15 | SDIO 命令 |

> ⚠ **UART 方向极易接反**：`WIFI_UART_TX_PIN` / `WIFI_UART_RX_PIN` 是**站在 C61 视角**
> 命名的（指的是 C61 自己的 TX0 / RX0）。配到 P4 这边要反过来：
> P4 的 **RX = GPIO8** ← C61 TX0，P4 的 **TX = GPIO9** → C61 RX0。

C61 的 IO9 是「复位时为低 → 进 UART 下载模式」的 strapping 脚。本板 IO9 是 **NC（未引出）**，
所以 P4 没法用 esptool 那套自动下载时序把 C61 拉进下载模式；只有飞线接到 TP5（GPIO4）时
才由 P4 控制。需要 P4 直接给 C61 烧固件时，在 `config.h` 里打开 `C61_IO9_PIN` 即可。

### 8. QSPI Flash

Flash 挂在 P4 的专用 MSPI（SPI0/1）总线上，硬件层直接映射，**不应**在用户代码里当普通 GPIO
配置。config.h 里只做引脚占用记录。

## 板级实现现状

板级类 `XiaoweiPlusBoard`（[xiaowei_plus.cc](xiaowei_plus.cc)）继承 `WifiBoard`，
构造函数按「分批私有方法」逐组 bring-up：

```
InitializePeriphI2c()  →  建外设 I2C 总线 + 扫总线
InitializePmic()       →  SY6206：充电配置 + 2s 喂狗/中断轮询定时器
InitializeImu()        →  LSM6DSV80X
InitializeHaptic()     →  AW86937
c61_.Init()            →  C61：复位 + 抓 3s 启动日志（C61Module 驱动）
InitializeButtons()    →  BOOT 键
InitializeTools()      →  MCP self.disp.network
```

**顺序有讲究**：外设 I2C 总线必须最先建（PMIC/IMU/马达都挂在它上面），PMIC 紧跟其后
（整板电源 + 看门狗，不能拖）。

| 功能 | 状态 |
| --- | --- |
| 网络（WiFi/蓝牙） | ✅ 走 `WifiBoard` + `esp_hosted`，板级 `.cc` 里**零** esp_hosted 调用 |
| 电量 | ✅ `GetBatteryLevel()` 真实现，走 SY6206（内阻补偿 + 放电曲线查表） |
| PMIC 看门狗 | ✅ 2s 定时器喂狗 + 读中断原因（插拔电源 / QON / 过温 / 过压…） |
| IMU | ✅ 初始化完成（120Hz / ±2g / ±250dps） |
| 振动马达 | ✅ 驱动就绪（初始化 + F0 测量）；但 `PlayHeartbeat()` 目前**没有调用方**，所以是哑的 |
| C61 诊断 | ✅ 复位 + UART 探针，能看出模块里有没有固件 |
| 显示 | ✅ `GetDisplay()` 返回 `MipiLcdDisplay`（CO5300 466x466 MIPI-DSI，见 §3）；面板起不来时退到无 UI 后端，绝不返回 `nullptr` |
| 触摸 | ✅ CST820B 注册为 LVGL 指针输入（点击可用；手势识别未移植） |
| 音频 | ❌ `GetAudioCodec()` 依赖 `NoAudioCodecSimplexPdm`（I2S 功放 + PDM 麦）；把参考工程 `board_audio.c` 的逻辑收进一个正经 `AudioCodec` 子类才算落地 |
| 背光 | ✅ `GetBacklight()` 返回 `Co5300Backlight`，亮度走面板 `0x51` 寄存器（本板无独立背光引脚）；屏没起来时返回 `nullptr` |
| LED | ❌ `GetLed()` 返回空实现 —— 硬件上就没有这颗灯（返回空对象而非 `nullptr`，因为框架无条件解引用） |

> ⚠ **当前状态：编得过、链接得过；除音频外都能跑。**
> `application.cc` 里有几处是**无条件解引用**的 —— `display->SetupUI()`、
> `audio_service_.Initialize(codec)`、`led->OnStateChanged()` —— 显示和 LED 已经用真实后端
> 或空实现兜住了，剩下音频这一处还需要一个正经的 `AudioCodec` 子类才能整机跑起来。

### QON（GPIO18）为什么固件没接管

三条原因，都不是偷懒：

1. P4 **只支持 EXT1 唤醒，没有 EXT0** —— `SOC_PM_SUPPORT_EXT0_WAKEUP` 未定义，
   `esp_sleep_enable_ext0_wakeup()` 在 P4 上根本编不出来。
2. P4 的 LP/RTC GPIO **只有 GPIO0~15**（`SOC_RTCIO_PIN_COUNT = 16`），QON 挂在 GPIO18，不在其中，
   做不了 EXT1 唤醒源。
3. QON 本来就是 SY6206 自己的电源键：长按硬关机、短按开机，PMIC 的硬件语义已经闭环，
   不需要 SoC 参与。

以后真要在固件里处理它，只有两条路：把 QON 挪到 LP GPIO 走 EXT1，或通过 I2C 让 PMIC 执行关机/深睡。

## 编译配置命令

**只需要两步 —— `set-target` 之后板级配置全部自动就位，不用手工改任何东西：**

```bash
idf.py set-target esp32p4     # 板级默认值自动合并进 sdkconfig
idf.py build
idf.py flash monitor
```

**menuconfig 里确认板子（按 target 已经默认选好了，一般不用动）：**

```
Xiaozhi Assistant -> Target Board -> WZKJ ESP32-P4 AI Xiaowei Plus (小未 Plus / C61 无线)
```

### 板级配置在哪、怎么隔离的

本工程只有 **low（S3）** 和 **plus（P4）** 两块板，各自一个 target，所以按 **target 隔离**：

| 文件 | 归属 |
| --- | --- |
| `sdkconfig.defaults` | 两块板**共用**（分区表、LVGL、mbedTLS、日志…） |
| `sdkconfig.defaults.esp32s3` | **只有 S3 low 板**（QIO 8MB PSRAM、S3 特有 cache/唤醒词…） |
| `sdkconfig.defaults.esp32p4` | **只有 P4 plus 板**（QIO flash、HEX 200M PSRAM、USB-Serial-JTAG 控制台、C61/esp-hosted 整套、网络调优…） |

`idf.py set-target <target>` 会自动把 `sdkconfig.defaults` + `sdkconfig.defaults.<target>`
合并成 `sdkconfig`；`main/Kconfig.projbuild` 里 Target Board 的默认值也是按 target 给的
（`default BOARD_TYPE_..._PLUS if IDF_TARGET_ESP32P4`）。所以**两块板互相看不见对方的配置**，
切换板子只要换 `set-target`，不残留、不冲突。

**这就是官方的分层，不是本工程的土办法。** 官方根目录同样有七份
`sdkconfig.defaults.<target>`；官方的 `sdkconfig.defaults.esp32p4` 里写着
`CONFIG_IDF_TARGET="esp32p4"` / `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` / `CONFIG_SPIRAM=y` /
**`CONFIG_SLAVE_IDF_TARGET_ESP32C6=y`**，给它的 **6 块 P4 板共用**；只有"某一块板独有"的东西
（SDIO 引脚）才写进那块板自己的 `config.json`（`m5stack/corep4` 放 44/43/45…、`m5stack/tab5`
放 8~13）。官方 `main/Kconfig.projbuild` 里**一条 esp_hosted / SLAVE_IDF_TARGET 配置都没有** ——
板子选完配置就位，靠的是这两层 defaults，不是 Kconfig `select`。

本工程只有一块板 / 一个 target，所以两层合并在 `sdkconfig.defaults.esp32p4` 一份里，等价于
官方"目标层 + 该板 config.json"的并集。

> ✅ **隔离性实测**（不是推理）：`idf.py -B build_s3 -D SDKCONFIG=sdkconfig.s3 set-target esp32s3`
> + 全量 build → exit 0。生成的 `sdkconfig.s3` 里 `ESP_HOSTED` / `SLAVE_IDF_TARGET` /
> `SPIRAM_MODE_HEX` 各 **0 条**，用的是 `BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW=y` +
> `SPIRAM_MODE_OCT=y`；S3 的组件表里 `esp_hosted` / `esp_wifi_remote` **完全不存在**
> （被 `idf_component.yml` 的 `rules: target in [esp32p4]` 挡掉），走原生 `esp_wifi`。
> 同时 P4 的 `sdkconfig` 没被动过。**改一块板不会污染另一块。**

### ⚠ 编译报 `CONFIG_WIFI_RMT_* was not declared` 怎么办

如果你看到这样的报错（在 `managed_components/78__esp-wifi-connect/wifi_manager.cc`）：

```
.../esp_wifi_remote/idf_v5.5/include/injected/esp_wifi.h:319: error:
    'CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM' was not declared in this scope
...:320: 'CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM' was not declared
...:321: 'CONFIG_WIFI_RMT_TX_BUFFER_TYPE' was not declared
...:324: 'CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF' was not declared
...:339: 'CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM' was not declared
```

**根因：环境变量 `ESP_IDF_VERSION` 不是「主.次」。** 和代码无关，和这几个值配没配也无关。

`espressif__esp_wifi_remote/Kconfig:10` 是

```
orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
```

它拼的是**文件名**，而 IDF 那边对应文件只按「主.次」命名（`Kconfig.idf_v5.5.in`）；
IDF 自己的 `IDF_VERSION` 是「主.次.补丁」（`build/config.env` 里写的就是 `"5.5.4"`）。
值一旦不是 `5.5`，`orsource` 作为**可选 source 不报错、静默跳过** → esp_hosted /
esp_wifi_remote 的整棵 Kconfig 子树消失：没有 `SLAVE_IDF_TARGET`、没有
`ESP_HOSTED_CP_TARGET_*`、没有 `WIFI_RMT_*`，SDIO 引脚那几条 `depends on` 也全不成立
→ 本板 `sdkconfig.defaults.esp32p4` 里对应的行被当成未知符号丢掉，slave target 退回
esp_hosted 的默认值 `esp32h2` + SPI。**但注入头文件照旧在 include path 上**
（`esp_wifi_remote/CMakeLists.txt` 是用 CMake 变量算那个目录的，跟这个环境变量无关），
于是 `WIFI_INIT_CONFIG_DEFAULT()` 里那几个宏全部未声明。

实测三档：

| `ESP_IDF_VERSION` | `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET` | `build/config/sdkconfig.h` 里 `WIFI_RMT` |
| --- | --- | --- |
| `5.5`（`export.ps1` 给的值） | `"esp32c61"` ✓ | 37 ✓ |
| `5.5.4`（IDE 集成终端 / 自建启动脚本） | `"esp32h2"` ✗ | **0** ← 就是这个报错 |
| 未设置 | — | kconfig 直接 `FATAL_ERROR ... Error 1` |

**工程已经自愈**：根 `CMakeLists.txt` 在 `project()` **之前**按 IDF 自己的版本覆盖了这个
变量（`include($ENV{IDF_PATH}/tools/cmake/version.cmake)` +
`set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")`），所以终端里是什么
值都不会再复发。那几行**不要删** —— `idf.py` 自己不设这个变量，完全靠外部 `export.ps1`。

**万一还是撞上（比如换了台机器、或这份补丁被回退）：**

```bash
grep -c "WIFI_RMT" build/config/sdkconfig.h     # 0 = 坏的；>0 = 正常
```

`sdkconfig` 里已有的值会**压过** `sdkconfig.defaults`（ESP-IDF 既定行为），所以光修好
环境变量还不够，被污染的 `sdkconfig` 必须重生成一次：

```bash
rm sdkconfig          # 或先备份成 sdkconfig.bak
idf.py set-target esp32p4
idf.py build
```

（官方 `scripts/build.py` 每次 reconfigure 前先把 `sdkconfig` 挪成 `sdkconfig.old`，
就是为了绕开这个粘性 —— 不是为了回滚。）

别去改 `wifi_manager.cc`，也别去手补那几个 `WIFI_RMT_*` 值 —— 那是治标。
（实测：干净 `set-target esp32p4` 生成的 sdkconfig 与已知良好的那份**逐字节一致**，111378 字节。）

> ⚠ 本 fork 的 `scripts/build.py` 对**任何**板子都跑不起来：它的
> `_build_option_definitions()` 会无条件去读 `DISPLAY_OLED_TYPE` / `DISPLAY_LCD_TYPE`
> 两个 choice，而 fork 的 `main/Kconfig.projbuild` 把它们裁掉了 → 抛
> `ValueError: Kconfig choice DISPLAY_OLED_TYPE was not found`。
> 所以板级配置**不要**依赖 `config.json` 的 `sdkconfig_append`（那条路要 build.py 才会应用，
> 走 `set-target` 时它是**不生效**的），统一写在 `sdkconfig.defaults.<target>` 里。

## 待办

- **UI 层**：显示驱动已经落地（`lcd.h/lcd.cc` + `touch.h/touch.cc`），但界面还是主工程
  自带的 LVGL UI。参考工程 `plus/lcd` 那套自定义 UI（`display/ui`、`apps`、`games`、
  `faces`、`emote_gen_player`）**没有移植**；要移植的话，圆屏安全区用 config.h 里的
  `DISPLAY_STATUS_BAR_TOP_OFFSET` / `DISPLAY_CHAT_BAR_WIDTH` / `DISPLAY_CHAT_BAR_BOTTOM_OFFSET`。
- **触摸手势**：只做了"指针输入"（点击/拖动交给 LVGL）。参考工程的手势识别
  （单击/双击/长按/滑动）在 `LcdTouch` 里没有实现，需要时再往 `ReadCb` 上叠一层。
- **音频驱动**：新建 `AudioCodec` 子类，把 I2S 功放（AW88082QNR）+ PDM 麦接进来。
- **蓝牙**：参考工程 `ai_toy_hwt` 用 Bluedroid + VHCI 把控制器跑在 C61 上
  （`CONFIG_ESP_HOSTED_BLUEDROID_HCI_VHCI=y`）。本工程目前没开 BT，需要时照抄。
- **马达**：接上 `PlayHeartbeat()` 的调用点（如按键反馈、提醒）。
- **摄像头**：DVP 引脚已在 config.h 里，驱动未落地；它和 CST820B 共用 `I2C_NUM_1`，
  到时候直接往 `LcdTouch` 建的那条总线上挂设备（或者把总线创建提到板级统一管理）。
- **`sy6206.cc` 里 `REG00` 未使用**：只是寄存器映射表的记录项，留着当文档。

## C61 无线是怎么配置的（以及一个反直觉的坑）

从机固件（esp-hosted 2.12.11）**已经烧进 C61**，P4 这边要做的就只是把 host 侧配置配对。
整套配置在 `sdkconfig.defaults.esp32p4` 里，`set-target` 自动生效，**板级 `.cc` 里一个
esp_hosted 调用都不需要写**——组件自己起 SDIO 链路，`esp_wifi_remote` 把标准的
`esp_wifi_*` 转发到 C61。

**关键顺序：必须先选对 slave target。** `esp_wifi_remote` 对 P4 把默认值硬编码成了 C6：

```
default SLAVE_IDF_TARGET_ESP32C6 if IDF_TARGET_ESP32P4   # To support common use-cases
```

不覆盖的话，`ESP_HOSTED_CP_TARGET_ESP32C61` 会因为 `depends on SLAVE_IDF_TARGET_ESP32C61`
不成立而**根本不出现在 menuconfig 里**，整条链路就按 C6 配，SDIO 引脚也全停在 esp_hosted
的默认值（CLK=18、CMD=19、D0~D3=14~17、RESET=54）——和本板一个都不对。所以两行都要写：

```
CONFIG_SLAVE_IDF_TARGET_ESP32C61=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61=y
```

生效后 `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c61"`，SDIO 六根线
（CLK=14 / CMD=15 / D0=13 / D1=12 / D2=11 / D3=10）和复位脚 GPIO7 也才是对的。

### ⚠ 复位极性：必须用 `ACTIVE_HIGH`，别想当然

C61 的 EN 是「**低 = 复位、高 = 运行**」，看起来该选 `ACTIVE_LOW` —— **错**。
esp-hosted 的复位序列是 `ACTIVE -> INACTIVE -> ACTIVE`，**最后停在 ACTIVE 状态**：

| 选项 | 结束时 GPIO7 电平 | 结果 |
| --- | --- | --- |
| `ACTIVE_HIGH` ✅ | 停在**高** | C61 正常运行 |
| `ACTIVE_LOW` ❌ | 停在**低** | C61 被一直按在复位里，SDIO 枚举超时 `0x107` |

所以正确写法是 `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`（也就是默认值）。
这条是本板参考实现实机验证过的结论。

