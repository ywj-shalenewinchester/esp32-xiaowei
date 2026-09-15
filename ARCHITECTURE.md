# 小智 AI 助手 — 工程架构说明

> 目标硬件：**WZKJ ESP32-S3 AI Xiaowei Low（小未低配版）**
> 固件版本：`PROJECT_VER = 2.4.2` ｜ 构建框架：ESP-IDF v5.5.4 ｜ 目标芯片：esp32s3
> 本文档描述当前工程**实际编译进固件**的代码结构。清理后仅保留此一块板卡。
>
> 📁 逐文件目录树见文末 **§13 附录**

---

## 目录

1. [硬件平台](#1-硬件平台)
2. [分层架构总览](#2-分层架构总览)
3. [T1 基础层 / 基类](#3-t1-基础层--基类)
4. [T2 HAL 层](#4-t2-hal-层)
5. [T3 应用层](#5-t3-应用层)
6. [关键继承链](#6-关键继承链)
7. [运行期数据流](#7-运行期数据流)
8. [启动流程](#8-启动流程)
9. [构建系统](#9-构建系统)
10. [当前编译配置](#10-当前编译配置)
11. [已编译但未使用的代码](#11-已编译但未使用的代码)
12. [已知文档失效](#12-已知文档失效)
13. [附录：逐文件目录树](#13-附录逐文件目录树)

---

## 1. 硬件平台

所有引脚定义集中在 `main/boards/wzkj/esp32-s3-ai-xiaowei_low/config.h`（纯 `#define`，无类无函数）。

| 子系统 | 器件 | 关键参数 |
|---|---|---|
| MCU | ESP32-S3 | 240 MHz，16 MB Flash，8 MB PSRAM（OCT @80 MHz） |
| 电源管理 | **SY6206** PMIC | I2C0：SCL=40, SDA=39, addr `0x6B`；PG=14, STAT=47, CE=38, INT=41, QON=21 |
| 音频编解码 | **ES8311** | I2C1：SCL=11, SDA=12, addr `0x30`；MCLK=10, SCLK=18, LRCK=19, DAC=9, ADC=8；采样率 24 kHz |
| 功放 | NS4150B | 使能脚 GPIO48 |
| 显示屏 | **ST77916** QSPI | **360×360 圆形**，SPI2_HOST；CLK=5, CS=17, RST=4, D0~D3=16/15/7/6；背光 BL=3 |
| 按键 | — | BOOT=GPIO0，电源键(QON)=GPIO21 |
| LED | GPIO13 | `R_LED_PIN`（低电平点亮 `R_LED_ACTIVE_LEVEL=0`；录音时 1Hz 闪烁）→ `GpioLed` |

**分区表** `partitions/16m.csv`（16 MB，双 OTA）：

```
nvs       data  nvs      0x9000      16 KB
otadata   data  ota      0xd000       8 KB
phy_init  data  phy      0xf000       4 KB
ota_0     app   ota_0    0x20000    0x3f0000
ota_1     app   ota_1    (自动)     0x3f0000
assets    data  spiffs   0x800000       8 MB   ← 字体/表情/声纹模型，运行时 mmap
```

无 `factory` 分区，只能走 OTA 槽位启动。

---

## 2. 分层架构总览

```
┌──────────────────────────────────────────────────────────────┐
│ T3  应用层 (Application)                                      │
│     main.cc · application · protocols/ · mcp_server · ota     │
│     —— 业务逻辑：对话流程、云端协议、AI 工具、固件升级           │
└──────────────────────────────────────────────────────────────┘
                            ↕ 通过抽象基类调用，不碰寄存器
┌──────────────────────────────────────────────────────────────┐
│ T2  HAL 层 (Hardware Abstraction)                             │
│  芯片驱动   : es8311_audio_codec · sy6206 · afe_audio_engine  │
│  板级装配   : xiaowei_low.cc · wifi_board · config.h          │
│  外设抽象   : i2c_device · backlight · button                  │
│  图形后端   : lcd_display · lvgl_display/ · gif/ · jpg/        │
└──────────────────────────────────────────────────────────────┘
                            ↕ 实现 T1 定义的接口
┌──────────────────────────────────────────────────────────────┐
│ T1  基础层 / 基类 (Foundation)                                 │
│  抽象接口   : Board · Display · AudioCodec · AudioEngine       │
│              WakeWord · Protocol · Led · Backlight             │
│              LvglFont · LvglImage · AssetStrategy              │
│  通用机制   : Settings(NVS) · SystemInfo · DeviceStateMachine  │
│              OggDemuxer · WakeWordAudioCache · TextGlyph       │
│              DynamicGlyphCache · text_glyph_payload            │
└──────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────┐
│ T0  构建与配置 (Build)                                         │
│     CMakeLists.txt · main/CMakeLists.txt · Kconfig.projbuild   │
│     sdkconfig.defaults* · partitions/16m.csv · scripts/*.py    │
└──────────────────────────────────────────────────────────────┘
```

**判层标准：**

- **T1 基础层** — 零硬件依赖、零业务语义。只有两种东西：**抽象接口基类**（定义"能做什么"，不知道"谁来做"）和**通用机制**（可被任何项目复用的算法/数据结构/持久化）。
- **T2 HAL 层** — 出现具体芯片型号、具体引脚、具体总线的地方。向上实现 T1 的接口，向下操作寄存器。
- **T3 应用层** — 出现产品语义的地方："唤醒词""对话""服务器""升级"。

**分层不是绝对的。** 少数文件横跨两层（如 `wifi_board.cc` 既是 HAL 又含少量业务策略），§4、§5 中会单独标注。

---

## 3. T1 基础层 / 基类

### 3.1 抽象接口基类

这些类**只定义接口**，具体实现全部在 T2。加新硬件 = 加一个新派生类，不改上层。

| 文件 | 抽象的类 | 纯虚函数 | 实现者 |
|---|---|---|---|
| `boards/common/board.h` | `Board` | 12 个：外设 `GetLed` `GetBacklight` `GetDisplay` `GetAudioCodec` `GetBatteryLevel`；网络与系统 `GetBoardType` `GetNetwork` `StartNetwork` `GetNetworkStateIcon` `SetPowerSaveLevel` `GetBoardJson` `GetDeviceStatusJson` | `WzkjBoard`（网络部分由 `WifiBoard` 代实现） |
| `display/display.h` | `Display` | `Lock` `Unlock` | `LvglDisplay`、`emote::EmoteDisplay` |
| `audio/audio_codec.h` | `AudioCodec` | `Read` `Write` | `Es8311AudioCodec` |
| `audio/audio_engine.h` | `AudioEngine` | 14 个（唤醒/AEC/VAD/编码上传） | `AfeAudioEngine` |
| `audio/wake_word.h` | `WakeWord` | 9 个 | `CustomWakeWord` |
| `protocols/protocol.h` | `Protocol` | `Start` `OpenAudioChannel` `CloseAudioChannel` `IsAudioChannelOpened` `SendAudio` `SendText` | `MqttProtocol`、`WebsocketProtocol` |
| `boards/common/backlight.h` | `Backlight` | `SetBrightnessImpl` | `PwmBacklight` |
| `boards/common/led.h` | `Led` | `OnStateChanged` | `GpioLed` |
| `display/lvgl_display/lvgl_font.h` | `LvglFont` | `font` `SetFallback` | `LvglBuiltInFont`、`LvglCBinFont` |
| `display/lvgl_display/lvgl_image.h` | `LvglImage` | `image_dsc` `IsGif` | `LvglRawImage`、`LvglCBinImage`、`LvglAllocatedImage` |
| `assets.h` | `AssetStrategy` | 资源加载策略 | `Assets::LvglStrategy`、`Assets::EmoteStrategy` |

**`Board::GetInstance()` 是整个工程的装配根**（`board.h:62`，内联静态单例）。它调用 `create_board()`，后者由 `xiaowei_low.cc` 末尾的 `DECLARE_BOARD(WzkjBoard)` 宏定义。**换板子只改这一个宏指向的类**。

### 3.2 通用机制

与硬件无关、可复用的基础设施。

| 文件 | 职责 |
|---|---|
| `settings.cc/.h` | NVS 键值持久化封装。按命名空间读写 string/int/bool，析构时 `nvs_commit`。全工程的配置持久化都走它。 |
| `system_info.cc/.h` | 系统信息静态工具类：Flash 大小、剩余堆、MAC、芯片型号、User-Agent、任务 CPU 占用、堆统计、PM 锁。 |
| `device_state.h` | 仅一个 `enum DeviceState`（11 个值），纯数据、无依赖。 |
| `device_state_machine.cc/.h` | 通用状态机（观察者模式）。内部维护**合法迁移表** `IsValidTransition()`，非法迁移打 warning 并拒绝；状态变更通过 `NotifyStateChange` 回调监听者。不含任何业务语义。 |
| `display/text_glyph.h/.cc` | 字形数据结构 `TextGlyph`（码点 + 度量 + 位图）+ PSRAM 优先的向量分配器。 |
| `display/lvgl_display/dynamic_glyph_cache.cc/.h` | 把运行期收到的 `TextGlyph` 批构建成 LVGL fallback 字体，带 LRU 淘汰（上限 256 字形 / 64 KB 位图）。 |
| `protocols/text_glyph_payload.cc/.h` | 解析云端 `glyph_push` 推送的 base64 字形位图，做严格合法性校验。 |
| `audio/demuxer/ogg_demuxer.cc/.h` | 极简 Ogg 容器解封装状态机，剥出裸 Opus 包。**纯格式解析，不碰硬件**，因此归 T1。 |
| `audio/wake_words/wake_word_audio_cache.cc/.h` | 唤醒词音频环形缓冲（PSRAM，约 2 秒 16 kHz PCM），线程安全覆盖写。 |
| `boards/common/i2c_device.cc/.h` | 通用 I2C 从设备基类（`WriteReg`/`ReadReg`/`ReadRegs`/`ResetBus`）。**位置在 HAL 目录但本身不含具体芯片**，是"给 HAL 用的工具"。 |

---

## 4. T2 HAL 层

### 4.1 芯片驱动

| 文件 | 职责 |
|---|---|
| `audio/codecs/es8311_audio_codec.cc/.h` | **ES8311 codec 驱动**。用 `esp_codec_dev` 组装 I2S `data_if` / I2C `ctrl_if` / `gpio_if`，创建 `es8311_codec_new`，实现双工读写、音量、PA 使能。依赖第三方 `esp_codec_dev`。 |
| `boards/wzkj/.../sy6206.cc/.h` | **SY6206 PMIC 驱动**（充电管理 + ADC）。继承 `I2cDevice`，封装寄存器 `REG00~REG37`、充电电压/电流配置、以及 Vbat/Vbus/Vsys/Pmid/Ibat/Ibus/Tdie/Ntc 八路 ADC 读数换算。 |
| `audio/engines/afe_audio_engine.cc/.h` | **ESP-SR AFE 引擎封装**。用单个 `esp_afe_sr` 实例统一 AEC + 唤醒词 + 语音处理；内部再起 `audio_afe` 任务做 `fetch_with_delay`。 |
| `audio/wake_words/custom_wake_word.cc/.h` | **MultiNet 自定义唤醒词**。从 `index.json` 解析语言/时长/门限/命令词，检测命中后触发回调。 |

### 4.2 板级装配

| 文件 | 职责 |
|---|---|
| `boards/wzkj/.../xiaowei_low.cc` | **本板卡的唯一装配点**。定义 `WzkjBoard`（初始化 PMIC / 音频 I2C / QSPI 圆屏 / 按键，重写 `GetAudioCodec` `GetDisplay` `GetBacklight` `GetLed` `GetBatteryLevel`）与 `WzkjLcdDisplay`（360×360 圆形安全区对齐）。末尾 `DECLARE_BOARD(WzkjBoard)`。 |
| `boards/wzkj/.../config.h` | 全部引脚、I2C 端口/地址、音频与显示参数的集中定义（见 §1）。 |
| `boards/wzkj/.../config.json` | 板卡元数据：`manufacturer=wzkj`、`type=esp32-s3-ai-xiaowei-low`、`target=esp32s3`。CMake 读取它生成 `BOARD_TYPE`/`BOARD_MANUFACTURER` 编译宏。 |
| `boards/common/board.cc` | `Board` 基类**仅剩**非外设的通用实现：UUID v4 生成并持久化、`GetSystemInfoJson()`（系统/芯片/分区/OTA/显示信息）。外设 getter 全部是纯虚，本文件不再提供任何默认实现。 |
| `boards/common/wifi_board.cc/.h` | `WifiBoard : public Board`，是 `WzkjBoard` 的直接父类，**本身是抽象类**（外设 getter 未实现，不可实例化）。WiFi 连接、60 秒超时、配网模式进入/退出、把 `WifiManager` 事件统一转成 `NetworkEvent`、网络状态图标、省电级别。**横跨 HAL/T3**：底层是 WiFi 驱动封装，但含"配网策略"业务逻辑。主程序从不按名字引用它，只经 `Board::GetInstance()` 多态分发。 |

### 4.3 外设抽象

| 文件 | 职责 |
|---|---|
| `boards/common/backlight.cc/.h` | `Backlight` 基类（亮度渐变定时器，5 ms 步进）+ `PwmBacklight`（LEDC 10 bit，`duty = 1023 × brightness / 100`）。亮度持久化到 Settings。 |
| `boards/common/led.cc/.h` | `Led` 基类（`OnStateChanged` 无参）+ `GpioLed`（单色 GPIO 灯，`esp_timer` 周期 500 ms 翻转，**仅 `kDeviceStateListening` 期间闪烁**，其余状态常灭；电平极性由构造参数给定）。 |
| `boards/common/button.cc/.h` | `Button` 封装 `iot_button` 事件（按下/松开/长按/单击/双击/多击）为 `std::function` 回调；另有 `AdcButton`、`PowerSaveButton`。 |
| `boards/common/i2c_device.cc/.h` | 通用 I2C 从设备基类（见 §3.2）。 |

### 4.4 图形后端

| 文件 | 职责 |
|---|---|
| `display/lcd_display.cc/.h` | 彩色 LCD 显示。含 `LcdDisplay` 及三种总线子类 `SpiLcdDisplay` / `RgbLcdDisplay` / `MipiLcdDisplay`；实现 LVGL 主题初始化、`esp_lvgl_port` 端口注册、两套 `SetupUI`（微信气泡式 vs 底栏字幕式）。 |
| `display/oled_display.cc/.h` | 单色 OLED 显示（128×64 / 128×32 两种布局）。**本工程不实例化**，见 §11。 |
| `display/emote_display.cc/.h` | emote 表情引擎显示，把 `Display` 接口映射到 `esp_emote_expression` 事件。**本工程不实例化**，见 §11。 |
| `display/lvgl_display/lvgl_display.cc/.h` | `LvglDisplay` — `Display` 与 `LcdDisplay`/`OledDisplay` 之间的**中间层**，复用状态栏/通知/电量/网络图标/截图/动态字形的公共 LVGL 逻辑。 |
| `display/lvgl_display/lvgl_theme.cc/.h` | `LvglTheme`（颜色/字体/背景图/表情集）+ `LvglThemeManager` 单例。`ParseColor` 解析 `#RRGGBB`。 |
| `display/lvgl_display/lvgl_font.cc/.h` | LVGL 字体抽象与内置字体、cbin 二进制字体加载。 |
| `display/lvgl_display/lvgl_image.cc/.h` | LVGL 图片抽象与三种子类（raw / cbin / allocated）。`LvglRawImage::IsGif()` 靠 "GIF" 魔数判断。 |
| `display/lvgl_display/emoji_collection.cc/.h` | 表情名 → `LvglImage*` 映射容器，析构时负责释放。 |
| `display/lvgl_display/gif/lvgl_gif.cc/.h` | **本工程自研**的 GIF 动画控制器（10 ms 定时器逐帧推进）。 |
| `display/lvgl_display/gif/gifdec.c/.h`<br>`gif/gifdec_mve.h` | **第三方移植**：public domain 的 gifdec 解码器，来自 LVGL。`gifdec_mve.h` 是 Helium/MVE 汇编优化（本平台不启用）。 |
| `display/lvgl_display/jpg/image_to_jpeg.cpp/.h` | **本工程自研适配**：图像 → JPEG 编码，支持软件（`esp_new_jpeg`）与硬件（`driver/jpeg_encode.h`）两条路径。用于屏幕截图。 |
| `display/lvgl_display/jpg/jpeg_to_image.c/.h` | JPEG → RGB565 解码。**无调用方**，见 §11。 |

---

## 5. T3 应用层

| 文件 | 职责 |
|---|---|
| `main.cc` | 唯一入口 `app_main()`。初始化 NVS（失败则擦除重试）→ `Application::GetInstance().Initialize()` → `.Run()`（永不返回）。 |
| `application.cc/.h` | **整个应用的总控**。持有 `DeviceStateMachine`、`AudioService`、`Protocol*`、`Ota`。实现主事件循环 `Run()`、14 个 `MAIN_EVENT_*` 事件处理（状态切换、开始/停止聆听、网络连断、唤醒词命中、激活完成…）、激活流程、版本检查、协议初始化、说话打断、固件升级、MCP 消息发送。 |
| `device_state_machine.cc/.h`<br>`device_state.h` | 状态机本体（归 T1）+ 状态枚举。**驱动方**：`Application::SetDeviceState()` ← 由事件处理器、激活任务、网络回调触发。 |
| `protocols/protocol.cc/.h` | `Protocol` 抽象基类（归 T1）+ 非纯虚部分实现：回调注册、`listen`/`abort`/`mcp` 文本消息构造与发送、超时判断。定义 `AudioStreamPacket`（贯穿所有音频队列的核心数据包）与二进制协议结构 `BinaryProtocol2`/`BinaryProtocol3`。 |
| `protocols/websocket_protocol.cc/.h` | WebSocket 信令 + 二进制音频帧实现。 |
| `protocols/mqtt_protocol.cc/.h` | MQTT 信令 + **UDP 加密音频通道**（AES-CTR 加解密、序列号防重放）。 |
| `mcp_server.cc/.h` | **MCP（Model Context Protocol，规范 2024-11-05）服务器**。JSON-RPC 2.0，把设备能力作为工具暴露给云端 AI。工具清单见 §5.1。 |
| `ota.cc/.h` | OTA 与激活。版本检查 → 固件下载（`esp_ota_begin/write/end` + 镜像校验）→ `set_boot_partition` → `MarkCurrentVersionValid` 取消回滚。激活动作用 efuse 序列号 + `HMAC_KEY0` 算 HMAC-SHA256 后 POST `{ota_url}/activate`。 |
| `assets.cc/.h` | **资源分区管理**。mmap 挂载 `assets` 分区，按策略模式（`LvglStrategy` / `EmoteStrategy`）加载 `index.json`、文本字体、emoji 集合、明暗主题/皮肤、声纹模型；支持从 URL 下载新资源包（边下边按扇区擦写 + magic/checksum 校验）。 |
| `assets/lang_config.h` | **自动生成**的多语言资源。`Lang::Strings::*`（约 40 条文案）+ `Lang::Sounds::OGG_*`（约 20 个音效，用 `asm("_binary_...")` 引用编译期嵌入的 ogg）。由 `scripts/gen_lang.py` 从 `assets/locales/<lang>/*.json` 生成。 |

### 5.1 MCP 工具清单

**AI 可见（Common Tools）：**
`self.get_device_status`、`self.audio_speaker.set_volume`、`self.screen.set_brightness`、`self.screen.set_theme`

**仅用户可见（User-Only Tools）：**
`self.get_system_info`、`self.reboot`、`self.upgrade_firmware`、`self.screen.get_info`、`self.screen.snapshot`、`self.screen.preview_image`、`self.assets.set_download_url`

### 5.2 云端消息类型

**设备 → 服务器**：`hello`、`listen`（`detect`/`start`/`stop`，`mode` 为 `realtime`/`auto`/`manual`）、`abort`、`mcp`、`goodbye`

**服务器 → 设备**（`Application::OnIncomingJson` 分发）：`tts`（`start`/`stop`/`sentence_start`）、`stt`、`llm`（`emotion`）、`mcp`、`system`（`command: reboot`）、`alert`、`display`（base64 图像）、`custom`

---

## 6. 关键继承链

```
Board (抽象, 12 纯虚：5 外设 + 7 网络/系统)
└─ WifiBoard                       boards/common/wifi_board.h       ← 仍抽象（不实现外设 getter）
   └─ WzkjBoard                    boards/wzkj/.../xiaowei_low.cc   ← DECLARE_BOARD, 实际实例

Display (抽象: Lock/Unlock)
├─ emote::EmoteDisplay             display/emote_display.h          ← 未实例化
└─ LvglDisplay (中间层)
   ├─ LcdDisplay
   │  ├─ SpiLcdDisplay
   │  │  └─ WzkjLcdDisplay         xiaowei_low.cc                   ← 实际实例
   │  ├─ RgbLcdDisplay                                              ← 未实例化
   │  └─ MipiLcdDisplay                                             ← 未实例化
   └─ OledDisplay                   display/oled_display.h           ← 未实例化

AudioCodec (抽象: Read/Write)
└─ Es8311AudioCodec                audio/codecs/es8311_audio_codec.h ← 实际实例

AudioEngine (抽象, 14 纯虚)
└─ AfeAudioEngine                   audio/engines/afe_audio_engine.h  ← 实际实例

WakeWord (抽象, 9 纯虚)             ← 被 AfeAudioEngine 内部持有
└─ CustomWakeWord                   audio/wake_words/custom_wake_word.h

Protocol (抽象)
├─ WebsocketProtocol                protocols/websocket_protocol.h
└─ MqttProtocol                     protocols/mqtt_protocol.h

Backlight (抽象: SetBrightnessImpl)  Led (抽象: OnStateChanged)
└─ PwmBacklight                     └─ GpioLed (GPIO 单色灯, 录音时闪烁)

I2cDevice
└─ Sy6206

LvglFont (抽象)                      LvglImage (抽象)
├─ LvglBuiltInFont                  ├─ LvglRawImage
└─ LvglCBinFont                     ├─ LvglCBinImage
                                    └─ LvglAllocatedImage

AssetStrategy (抽象)
├─ Assets::LvglStrategy             ← 当前生效 (HAVE_LVGL=1)
└─ Assets::EmoteStrategy
```

**持有关系（组合）：**

```
Application
├── DeviceStateMachine  state_machine_
├── AudioService        audio_service_
│   ├── AudioCodec*     audio_codec_      (裸指针，不拥有 ← Board 提供)
│   ├── AudioEngine     audio_engine_     (unique_ptr)
│   │   └── CustomWakeWord  custom_wake_word_  (unique_ptr)
│   │       └── WakeWordAudioCache
│   ├── AudioDebugger   (unique_ptr)
│   ├── OggDemuxer
│   └── 5 队列 + 3 任务
├── Protocol*           protocol_
└── Ota                 ota_
```

---

## 7. 运行期数据流

### 7.1 音频上行（说话 → 云端）

```
ES8311 ──I2S──> AudioCodec::Read
   │
   ▼  [audio_input 任务, 优先级 8]
AfeAudioEngine::Feed  ──> esp_afe_sr ──> VAD / AEC / WakeNet
   │
   ▼  [audio_encode_queue_]
   │
   ▼  [opus_codec 任务, 优先级 2]  PCM → Opus
   │
   ▼  [audio_send_queue_]
Protocol::SendAudio ──> WebSocket 二进制帧 / UDP+AES-CTR
```

### 7.2 音频下行（云端 → 喇叭）

```
服务器 ──> Protocol::OnIncomingAudio ──> AudioStreamPacket
   │
   ▼  [audio_decode_queue_]
   │
   ▼  [opus_codec 任务]  Opus → PCM
   │
   ▼  [audio_playback_queue_]
   │
   ▼  [audio_output 任务, 优先级 4]
AudioCodec::Write ──I2S──> ES8311 ──> NS4150B ──> 喇叭
```

### 7.3 唤醒词触发

```
WakeNet 命中 → AudioEngine::OnWakeWordDetected
   ├─ WakeWordAudioCache 里"唤醒瞬间之前"的 PCM 编码为 Opus 上传（CONFIG_SEND_WAKE_WORD_DATA=y）
   └─ Application 收到 → 打断当前播放 → 切到 Listening
```

### 7.4 运行期任务一览

| 任务 | 优先级 | 归属 | 职责 |
|---|---|---|---|
| `audio_input` | 8 | AudioService | 读 codec，喂引擎 |
| `audio_output` | 4 | AudioService | 写 codec，放音 |
| `opus_codec` | 2 | AudioService | **单任务同时做编解码** |
| `audio_afe` | 3 | AfeAudioEngine | `fetch_with_delay` 取 AFE 结果 |
| `encode_wake_word` | — | 按需 `xTaskCreateStatic` | 唤醒词音频编码上传 |
| `audio_power_timer` | — | esp_timer 1 s | 功耗状态检查 |

同步：`event_group_`（4 个事件位）+ 队列互斥量 + 条件变量 + 5 条 `std::deque`。

---

## 8. 启动流程

```
app_main()                                          main.cc
 ├─ nvs_flash_init()  (失败 → erase → 重试)
 ├─ Application::GetInstance()
 └─ Application::Initialize()                       application.cc
     ├─ Board::GetInstance()  ★ 触发 WzkjBoard 构造
     │    ├─ InitializePmic()         SY6206 @ I2C0
     │    ├─ InitializeAudioI2c()     I2C1
     │    ├─ InitializeLcdDisplay()   ST77916 QSPI + WzkjLcdDisplay
     │    ├─ InitializeButtons()      BOOT / QON
     │    └─ InitializeTools()        注册板级 MCP 工具
     ├─ SetDeviceState(kDeviceStateStarting)
     ├─ display->SetupUI() + 显示 UserAgent
     ├─ audio_service_.Initialize(codec) + Start()   ← 起 3 个任务
     │    └─ 注入 4 个回调: send_queue_available / wake_word / vad_change / playback_drained
     ├─ state_machine_.AddStateChangeListener(...)
     ├─ clock_timer (1 s 周期, 刷新状态栏)
     ├─ McpServer::AddCommonTools() + AddUserOnlyTools()
     ├─ board.SetNetworkEventCallback(...)
     ├─ board.StartNetwork()      ★ 异步联网，返回后状态可能仍是 Starting
     └─ display->UpdateStatusBar(true)
 └─ Application::Run()   ← 主事件循环，永不返回
```

**协议选择**（`Application::InitializeProtocol()`）：
OTA 返回 `HasMqttConfig()` → `MqttProtocol`；否则 `HasWebsocketConfig()` → `WebsocketProtocol`；两者皆无 → 默认 `MqttProtocol`。

**设备状态（11 个）**：`Unknown` `Starting` `WifiConfiguring` `Idle` `Connecting` `Listening` `Speaking` `Upgrading` `Activating` `AudioTesting` `FatalError`

---

## 9. 构建系统

### 9.1 文件

| 文件 | 职责 |
|---|---|
| `CMakeLists.txt`（顶层） | 引入 ESP-IDF `project.cmake`，`MINIMAL_BUILD ON`，`PROJECT_VER=2.4.2`，`project(xiaozhi)`。 |
| `main/CMakeLists.txt` | 显式列出 32 个源文件 + 5 个 `boards/common`；GLOB 板卡目录 `*.cc`；追加 AFE 引擎与唤醒词 3 个文件。**当前总计编译 42 个 .cc/.c/.cpp**。也定义资源生成规则。 |
| `main/Kconfig.projbuild` | 全部可配置项，见 §10。 |
| `sdkconfig.defaults` | 通用默认：16 MB Flash、自定义分区表、C++ 异常/RTTI、LVGL 精简裁剪、mbedTLS 动态缓冲。 |
| `sdkconfig.defaults.esp32s3` | S3 专属：QIO flash、240 MHz、8 MB OCT PSRAM、`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`。 |
| `partitions/16m.csv` | 分区表，见 §1。 |
| `.vscode/settings.json` | IDF 路径、COM13、clangd 指向 `${workspaceFolder}/build`。 |
| `.clangd` / `.clang-format` | 编译标志过滤、代码风格。 |

### 9.2 脚本

| 脚本 | 行数 | 职责 |
|---|---|---|
| `scripts/gen_lang.py` | 186 | 从 `assets/locales/<lang>/` 生成 `main/assets/lang_config.h`。构建期由 CMake 调用。 |
| `scripts/build_default_assets.py` | 937 | 生成 `built/generated_assets.bin`（字体 + emoji + 声纹模型 + 主题），烧到 `assets` 分区。 |
| `scripts/build.py` | 1852 | 上层构建/发布编排（多板卡批量、版本管理）。**当前单板卡流程未直接调用。** |
| `scripts/versions.py` | 249 | 版本号工具。 |

### 9.3 资源

- `main/assets/locales/` — **38 种语言**，每种约 16 个 `.ogg` 音效 + 文案 JSON。
- `main/assets/common/` — 5 个跨语言通用音效：`exclamation` `low_battery` `popup` `success` `vibration`。

---

## 10. 当前编译配置

来自 `sdkconfig` 实际生效值：

| 配置项 | 值 | 含义 |
|---|---|---|
| `CONFIG_BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW` | y | 目标板（唯一选项） |
| `CONFIG_USE_AFE_WAKE_WORD` | **y** | 唤醒词走 WakeNet + AFE |
| `CONFIG_USE_CUSTOM_WAKE_WORD` | n | MultiNet 自定义唤醒词**未启用**（代码仍在） |
| `CONFIG_USE_AUDIO_PROCESSOR` | y | 启用 AFE 音频前端 |
| `CONFIG_USE_DEVICE_AEC` | n | 设备端 AEC 关闭 |
| `CONFIG_USE_SERVER_AEC` | n | 服务器端 AEC 关闭 |
| `CONFIG_SEND_WAKE_WORD_DATA` | y | 唤醒词音频上传服务器 |
| `CONFIG_USE_AUDIO_DEBUGGER` | n | UDP 音频调试关闭 |
| `CONFIG_USE_DEFAULT_MESSAGE_STYLE` | y | 底栏字幕式 UI（非微信气泡） |
| `CONFIG_FLASH_DEFAULT_ASSETS` | y | 烧录默认资源包 |
| `CONFIG_USE_HOTSPOT_WIFI_PROVISIONING` | y | 热点配网 |
| `CONFIG_OTA_URL` | `https://api.tenclass.net/xiaozhi/ota/` | OTA / 激活服务器 |

> `App::InitializeProtocol()` 的实际选择由该 OTA 服务器返回的配置决定，**编译期不确定**。

---

## 11. 已编译但未使用的代码

以下代码**在 `SOURCES` 里、会被编译进固件**，但运行期从不执行。清理时可作为候选，删除前请确认无外部依赖。

| 文件 | 状态 | 证据 |
|---|---|---|
| `display/oled_display.cc/.h` | 编译，**从不实例化** | 全工程无 `new OledDisplay`；仅 `board.cc` / `mcp_server.cc` 用 `dynamic_cast<OledDisplay*>` 探测单色屏 |
| `display/emote_display.cc/.h` | 编译，**从不实例化** | 无 `new emote::EmoteDisplay`；仅 `assets.cc` 在 `#if !HAVE_LVGL` 分支做 `dynamic_cast`，当前 `HAVE_LVGL=1` 走不到 |
| `display/lvgl_display/jpg/jpeg_to_image.c/.h` | 编译，**零调用方** | 全工程无任何调用 `jpeg_to_image()` |
| `display/lcd_display.h` 的 `RgbLcdDisplay` / `MipiLcdDisplay` | 编译，**从不实例化** | 仅 `SpiLcdDisplay` 被 `WzkjLcdDisplay` 继承使用 |
| `audio/audio_debugger.cc/.h` | 编译，运行时空操作 | 全部包在 `CONFIG_USE_AUDIO_DEBUGGER` 内，当前为 n |
| `audio/wake_word.h` + `custom_wake_word.cc` | 编译，**当前配置下不启用** | `CONFIG_USE_AFE_WAKE_WORD=y` 走 WakeNet；`AfeAudioEngine` 内 `WakeDetector::kMultiNet` 分支不激活 |
| `scripts/build.py` | 存在 | 单板卡流程未调用（`main/CMakeLists.txt` 只用 `gen_lang.py` 和 `build_default_assets.py`） |
| `display/lvgl_display/gif/gifdec_mve.h` | 编译条件不满足 | Helium/MVE 为 ARM 扩展，ESP32-S3 不启用 |

---

## 12. 已知文档失效

工程内残留的 `README.md` **描述的是清理前的多板卡版本**，与实际代码不符，请勿依据它们理解本工程：

| 文件 | 失效内容 |
|---|---|
| `main/boards/wzkj/esp32-s3-ai-xiaowei_low/README.md` | 板卡说明，需按 `config.h` 实际值核对。 |

> `main/audio/README.md`（内容描述 es8374/es8388/lite_audio_engine 等不存在的文件）和 `main/display/lvgl_display/gif/README.md` 已随清理删除。

---

*文档基于清理后的工程实际代码生成。分层判定以"是否出现具体硬件/引脚"和"是否出现产品语义"两条标准划分，个别文件横跨两层时已在正文标注。*

---

## 13. 附录：逐文件目录树

### 图例

| 标记 | 含义 |
|---|---|
| `[T1]` | **基础层** — 抽象接口基类 / 通用机制，零硬件、零业务 |
| `[T2]` | **HAL 层** — 芯片驱动、板级装配、外设抽象、图形后端 |
| `[T3]` | **应用层** — 业务逻辑（对话、协议、AI 工具、升级） |
| `[T0]` | 构建与配置（非 C++ 代码） |
| `⚙️` | **构建期生成** — 由脚本生成，勿手改 |
| `⚠️` | **已编译但未使用** — 见 §11 |
| `📦` | 第三方 / 移植代码 |
| `▲` | 需要留意的异常 |

### 目录树

```
xiaozhi/  (esp32-xiaowei-delete)
│
├── CMakeLists.txt  [T0] 顶层构建入口：MINIMAL_BUILD ON、PROJECT_VER=2.4.2、project(xiaozhi)
├── sdkconfig  [T0] ⚙️ IDF 生成的完整配置（勿手改）
├── sdkconfig.old  [T0] ⚙️ 上次配置备份，可删
├── sdkconfig.defaults  [T0] 通用默认值：16MB Flash、自定义分区表、C++ 异常/RTTI、LVGL 精简裁剪
├── sdkconfig.defaults.esp32s3  [T0] S3 专属：QIO、240MHz、8MB OCT PSRAM、
                                     CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
├── dependencies.lock  [T0] ⚙️ 组件管理器锁定文件，27 项依赖
├── partitions/
│   └── 16m.csv  [T0] 16MB 双 OTA 分区表（nvs/otadata/phy_init/ota_0/ota_1/assets 8MB）
├── scripts/
│   ├── gen_lang.py  [T0] 生成 main/assets/lang_config.h（构建期调用）
│   ├── build_default_assets.py  [T0] 生成 generated_assets.bin（字体+emoji+声纹+主题，
                                      烧到 assets 分区）
│   ├── build.py  [T0] 上层构建/发布编排（单板卡流程未调用）
│   └── versions.py  [T0] 版本号工具
├── .vscode/
│   └── settings.json  [T0] IDF 路径、COM13、clangd → ${workspaceFolder}/build
├── .clangd  [T0] 过滤 -f*/-m* 编译标志
├── .clang-format  [T0] C++ 代码风格
├── .gitignore  [T0] 忽略 sdkconfig / build / managed_components / lang_config.h 等生成物
├── ARCHITECTURE.md  [T0] 本文件：架构说明 + 逐文件目录树
├── build/  ⚙️ 编译产物（xiaozhi.bin / xiaozhi.elf）
├── managed_components/  📦 组件管理器下载的 26 个第三方组件（593MB）
└── main/  应用主组件
    ├── CMakeLists.txt  [T0] 显式列出 32 个源文件 + 5 个 boards/common；GLOB 板卡目录；
                             追加 AFE 引擎与唤醒词；定义资源生成规则
    ├── Kconfig.projbuild  [T0] 全部可配置项（板卡、显示样式、唤醒词、AEC、调试、配网方式）
    ├── idf_component.yml  [T0] 精简后的依赖清单（14 个直接依赖 + idf）
    ├── main.cc  [T3] 唯一入口 app_main()：初始化 NVS → Application::Initialize() → Run()
    ├── application.h/.cc  [T3] ★ 应用总控：主事件循环、14 个事件处理、激活流程、
                                协议初始化、说话打断、固件升级
    ├── device_state.h  [T1] enum DeviceState（11 个状态值），纯数据
    ├── device_state_machine.h/.cc  [T1] 通用状态机（观察者模式）+ 合法迁移表
                                         IsValidTransition()，不含业务语义
    ├── settings.h/.cc  [T1] NVS 键值持久化封装（namespace + commit）
    ├── system_info.h/.cc  [T1] 系统信息工具：Flash、堆、MAC、芯片型号、User-Agent、
                                任务 CPU 占用
    ├── assets.h/.cc  [T3] 资源分区管理：mmap 挂载 assets 分区、策略模式加载
                           字体/表情/主题/声纹、URL 下载
    ├── ota.h/.cc  [T3] OTA + 激活：版本检查、固件下载校验、HMAC-SHA256 激活、取消回滚
    ├── mcp_server.h/.cc  [T3] MCP 服务器（JSON-RPC 2.0，规范 2024-11-05），把设备能力
                               作为工具暴露给云端 AI
    ├── assets/
    │   ├── lang_config.h  [T3] ⚙️ 自动生成的多语言资源（Lang::Strings::* +
                                Lang::Sounds::OGG_*），由 gen_lang.py 生成
    │   ├── common/  5 个跨语言通用音效
    │   │   ├── exclamation.ogg  "！"提示音
    │   │   ├── low_battery.ogg  低电量提示
    │   │   ├── popup.ogg  弹窗提示
    │   │   ├── success.ogg  操作成功
    │   │   └── vibration.ogg  振动提示
    │   └── locales/  38 种语言包，每种 17 个文件：0.ogg~9.ogg 十个数字音效 +
                      activation/err_pin/err_reg/upgrade/welcome/wificonfig
                      6 个 .ogg + language.json（文案）
    │       ├── ar-SA/  阿拉伯语      ├── ja-JP/  日语
    │       ├── bg-BG/  保加利亚语    ├── ko-KR/  韩语
    │       ├── ca-ES/  加泰罗尼亚语  ├── ms-MY/  马来语
    │       ├── cs-CZ/  捷克语        ├── nb-NO/  挪威语
    │       ├── da-DK/  丹麦语        ├── nl-NL/  荷兰语
    │       ├── de-DE/  德语          ├── pl-PL/  波兰语
    │       ├── el-GR/  希腊语        ├── pt-PT/  葡萄牙语
    │       ├── en-US/  英语(回退源)  ├── ro-RO/  罗马尼亚语
    │       ├── es-ES/  西班牙语      ├── ru-RU/  俄语
    │       ├── fa-IR/  波斯语        ├── sk-SK/  斯洛伐克语
    │       ├── fi-FI/  芬兰语        ├── sl-SI/  斯洛文尼亚语
    │       ├── fil-PH/  菲律宾语     ├── sr-RS/  ! 塞尔维亚语，无 ogg
    │       ├── fr-FR/  法语          ├── sv-SE/  瑞典语
    │       ├── he-IL/  希伯来语      ├── th-TH/  泰语
    │       ├── hi-IN/  印地语        ├── tr-TR/  土耳其语
    │       ├── hr-HR/  克罗地亚语    ├── uk-UA/  乌克兰语
    │       ├── hu-HU/  匈牙利语      ├── vi-VN/  越南语
    │       ├── id-ID/  印尼语        ├── zh-CN/  简体中文 ← 当前使用
    │       ├── it-IT/  意大利语      └── zh-TW/  繁体中文
    ├── audio/
    │   ├── audio_codec.h/.cc  [T1] AudioCodec 抽象基类（纯虚 Read/Write）+ 默认实现
                                    （音量持久化、输入输出使能）
    │   ├── audio_engine.h  [T1] AudioEngine 纯虚抽象（14 个纯虚，统一 AEC/唤醒词/
                                 语音处理接口）
    │   ├── wake_word.h  [T1] WakeWord 纯虚抽象（9 个纯虚）
    │   ├── audio_service.h/.cc  [T1] ★ 音频调度中枢：持有 codec/引擎/调试器/Opus
                                      编解码器，5 条队列 + 3 个 RTOS 任务
    │   ├── audio_debugger.h/.cc  [T2] ⚠️ UDP 音频调试器（CONFIG_USE_AUDIO_DEBUGGER
                                       当前=n，运行时空操作）
    │   ├── codecs/
    │   │   └── es8311_audio_codec.h/.cc  [T2] ES8311 codec 驱动：esp_codec_dev 组装
                                               I2S data_if / I2C ctrl_if / gpio_if
    │   ├── demuxer/
    │   │   └── ogg_demuxer.h/.cc  [T1] Ogg 容器解封装状态机，剥出裸 Opus 包
                                        （纯格式解析，不碰硬件）
    │   ├── engines/
    │   │   └── afe_audio_engine.h/.cc  [T2] ESP-SR AFE 引擎封装：单实例统一 AEC+唤醒词+
                                             语音处理，内部起 audio_afe 任务
    │   └── wake_words/
    │       ├── custom_wake_word.h/.cc  [T2] MultiNet 自定义唤醒词（当前
                                             CONFIG_USE_AFE_WAKE_WORD=y，此路径不激活）
    │       └── wake_word_audio_cache.h/.cc  [T1] 唤醒词音频环形缓冲（PSRAM，约 2 秒
                                                  16kHz PCM），线程安全覆盖写
    ├── boards/
    │   ├── common/  跨板卡可复用组件
    │   │   ├── board.h/.cc  [T1] ★ Board 抽象基类（12 个纯虚：5 外设 + 7 网络/系统）
                                  + Board::GetInstance() 装配根 + DECLARE_BOARD 宏
                                  （.cc 只留 UUID 与 GetSystemInfoJson）
    │   │   ├── wifi_board.h/.cc  [T2] WifiBoard : Board —— 仍抽象；WiFi 连接、60s 超时、
                                       配网模式、事件转发为 NetworkEvent
                                       （横跨 T2/T3：含配网策略）
    │   │   ├── backlight.h/.cc  [T2] Backlight 抽象（亮度渐变定时器）+ PwmBacklight
                                      （LEDC 10bit）
    │   │   ├── led.h/.cc  [T2] Led 抽象 + GpioLed（录音时 1Hz 闪烁，其余状态常灭）
    │   │   ├── button.h/.cc  [T2] Button 封装 iot_button 事件
                                   （按下/长按/单击/双击/多击）
    │   │   └── i2c_device.h/.cc  [T1] 通用 I2C 从设备基类（WriteReg/ReadReg...）
    │   └── wzkj/
    │       └── esp32-s3-ai-xiaowei_low/  ★ 唯一板卡
    │           ├── README.md  [T0] 板卡说明（需按 config.h 实际值核对）
    │           ├── config.h  [T2] 全部引脚/I2C 端口与地址/音频与显示参数
    │           ├── config.json  [T2] 板卡元数据（manufacturer=wzkj,
                                      type=esp32-s3-ai-xiaowei-low, target=esp32s3）
    │           ├── xiaowei_low.cc  [T2] ★★ 唯一装配点：WzkjBoard（PMIC/音频 I2C/QSPI 圆屏/
                                         按键/LED）+ WzkjLcdDisplay +
                                         DECLARE_BOARD(WzkjBoard)
    │           └── sy6206.h/.cc  [T2] SY6206 PMIC 驱动（充电管理 + 8 路 ADC），
                                       继承 I2cDevice，I2C0 addr 0x6B
    ├── display/
    │   ├── display.h/.cc  [T1] Display 抽象基类（纯虚 Lock/Unlock）+ Theme 基类 +
                                DisplayLockGuard
    │   ├── text_glyph.h/.cc  [T1] TextGlyph 数据结构（码点+度量+位图）+
                                   PSRAM 优先分配器
    │   ├── lcd_display.h/.cc  [T2] 彩色 LCD：LcdDisplay + SpiLcdDisplay /
                                    RgbLcdDisplay / MipiLcdDisplay（后两者 ⚠️ 未实例化）；
                                    两套 SetupUI（气泡式/底栏式）
    │   ├── oled_display.h/.cc  [T2] ⚠️ 单色 OLED（128x64/128x32）—— 全工程 0 处
                                     new OledDisplay，仅被 dynamic_cast 探测
    │   ├── emote_display.h/.cc  [T2] ⚠️ emote 表情引擎显示 —— 0 处实例化，仅在
                                      #if !HAVE_LVGL 分支被探测（当前走不到）
    │   └── lvgl_display/
    │       ├── lvgl_display.h/.cc  [T2] LvglDisplay 中间层：状态栏/通知/电量/网络图标/
                                         截图/动态字形等公共 LVGL 逻辑
    │       ├── lvgl_theme.h/.cc  [T2] LvglTheme（颜色/字体/背景/表情集）+
                                       LvglThemeManager 单例
    │       ├── lvgl_font.h/.cc  [T2] LvglFont 抽象 + LvglBuiltInFont +
                                      LvglCBinFont（cbin 二进制字体）
    │       ├── lvgl_image.h/.cc  [T2] LvglImage 抽象 + Raw/CBin/Allocated 三子类
    │       ├── emoji_collection.h/.cc  [T2] 表情名 → LvglImage* 映射容器
    │       ├── dynamic_glyph_cache.h/.cc  [T1] TextGlyph → LVGL fallback 字体，LRU 淘汰
                                                （≤256 字形 / 64KB 位图）
    │       ├── gif/
    │       │   ├── lvgl_gif.h/.cc  [T2] GIF 动画控制器（10ms 定时器逐帧推进）
    │       │   ├── gifdec.h/.c  📦 gifdec 解码器（public domain，移植自 LVGL）
    │       │   ├── gifdec_mve.h  📦 ⚠️ Helium/MVE 汇编优化 —— ESP32-S3 不启用
    │       │   └── LICENSE.txt  [T0] public domain 声明
    │       └── jpg/
    │           ├── image_to_jpeg.h/.cpp  [T2] 图像 → JPEG 编码（软件 esp_new_jpeg +
                                               可选硬件 driver/jpeg_encode.h），用于截图
    │           └── jpeg_to_image.h/.c  [T2] ⚠️ JPEG → RGB565 解码 —— 全工程 0 处调用
    └── protocols/
        ├── protocol.h/.cc  [T1] Protocol 抽象基类 + AudioStreamPacket（贯穿所有音频
                                 队列的核心包）+ 二进制协议结构
        ├── websocket_protocol.h/.cc  [T3] WebSocket 信令 + 二进制音频帧
        ├── mqtt_protocol.h/.cc  [T3] MQTT 信令 + UDP 加密音频通道（AES-CTR + 序列号防重放）
        └── text_glyph_payload.h/.cc  [T1] 解析云端 glyph_push 的 base64 字形位图，
                                           严格合法性校验
```

### 依赖组件（`managed_components/` 26 个）

由 `main/idf_component.yml` 声明、组件管理器自动解析下载。**不是手写代码，勿改；删掉会重新下载 593MB。**

| 组件 | 用途 |
|---|---|
| `78__esp-ml307` | 网络抽象层（NetworkInterface / http / mqtt / udp），被 `board.h`、`wifi_board.cc` 使用 |
| `78__esp-wifi-connect` | WiFi 管理器、配网、SSID 管理 |
| `78__uart-uhci` | UART 驱动（esp-wifi-connect 依赖） |
| `78__xiaozhi-fonts` | 内置字体资源（`xiaozhi-fonts` 也在 CMake `PRIV_REQUIRES` 中） |
| `espressif__esp-sr` | **语音识别框架**：AFE / WakeNet / MultiNet（272MB，最大依赖） |
| `espressif__esp_audio_codec` | Opus 编解码 |
| `espressif__esp_audio_effects` | 音频效果（AEC 等） |
| `espressif__esp_codec_dev` | codec 设备抽象框架（ES8311 驱动基于它） |
| `espressif__esp_image_effects` | 图像效果处理 |
| `espressif__esp_new_jpeg` | JPEG 软件编解码 |
| `lvgl__lvgl` | **LVGL 9.5 图形库**（182MB） |
| `espressif__esp_lvgl_port` | LVGL 与 IDF 的对接端口 |
| `espressif__esp_lcd_st77916` | ST77916 屏幕驱动（本板 QSPI 圆屏） |
| `espressif__esp_lcd_touch` | 触摸屏驱动（本板未用触摸，但属传递依赖） |
| `espressif2022__esp_emote_expression` | emote 表情引擎（`emote_display.cc` 用，当前未实例化） |
| `espressif2022__esp_emote_gfx` | emote 图形后端（传递依赖） |
| `espressif2022__esp_emote_assets` | emote 默认资源（传递依赖） |
| `espressif__button` | 按键组件（`iot_button`，Button 类基于它） |
| `espressif__cjson` | JSON 解析（协议层和 MCP 全靠它） |
| `espressif__esp-dsp` | DSP 加速（音频处理） |
| `espressif__esp_mmap_assets` | 资源分区 mmap 挂载（`assets.cc` 用它） |
| `espressif__freetype` | 字体渲染（传递依赖） |
| `espressif__dl_fft` / `espressif__gmf_fft` | FFT 加速（esp-sr 的传递依赖） |
| `espressif__cmake_utilities` | 构建工具（其他组件的传递依赖） |
| `laride__heatshrink` | 压缩算法（传递依赖） |

> `espressif/mqtt` 在 `dependencies.lock` 里但**不在本目录**——它由 ESP-IDF 自带（`$IDF_PATH/components/mqtt`）。

### 需要留意的问题

| 位置 | 问题 |
|---|---|
| `main/assets/locales/sr-RS/` | **语言包残缺**：只有 `language.json`，无任何 `.ogg`。CMake 的 en-US 回退逻辑会让它使用英文提示音，所以不会构建失败，但语音是英文、文字是塞尔维亚语。 |
| `main/CMakeLists.txt` | `SOURCES` 里含 6 项已编译但从不执行的代码（oled_display / emote_display / jpeg_to_image / RgbLcdDisplay / MipiLcdDisplay / gifdec_mve.h），见 §11。 |
| `main/application.h` | `TaskPriorityReset` 类原本只被相机拍照工具使用，相机链路删除后已无调用方，可一并清理。 |
| `sdkconfig.old` | IDF 每次重新配置时生成的备份，可直接删除。 |
| `scripts/build.py` | 1852 行的多板卡构建编排脚本，当前单板卡流程未调用。 |
