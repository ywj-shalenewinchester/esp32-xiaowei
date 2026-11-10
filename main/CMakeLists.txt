# Define source files
set(SOURCES "audio/audio_codec.cc"
            "audio/audio_debugger.cc"
            "audio/audio_service.cc"
            "audio/demuxer/ogg_demuxer.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/no_audio_codec.cc"
            "display/display.cc"
            "display/text_glyph.cc"
            "display/lcd_display.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/emote_display.cc"
            "display/lvgl_display/emoji_collection.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/dynamic_glyph_cache.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/gif/lvgl_gif.cc"
            "display/lvgl_display/gif/gifdec.c"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/text_glyph_payload.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "device_state_machine.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols")

# Add board common files
list(APPEND SOURCES
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/backlight.cc"
    "boards/common/led.cc"
    "boards/common/button.cc"
    "boards/common/i2c_device.cc"
)
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)


idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Target board selection.
#
# One mutually exclusive branch per supported board, keyed off the Kconfig
# symbol from `choice BOARD_TYPE`. Because exactly one branch can match, only
# one board directory is ever globbed into SOURCES -- that is what lets two
# boards coexist in this repo and still be built completely independently.
#
# scripts/build.py parses this exact `if(CONFIG_BOARD_TYPE_*) -> set(BOARD_DIR ...)`
# shape (see its _find_board_config_candidates) to resolve which Kconfig symbol
# belongs to a board directory, so every branch added here must have a matching
# `config BOARD_TYPE_*` in main/Kconfig.projbuild.
#
# BUILTIN_*_FONT and DEFAULT_EMOJI_COLLECTION below are consumed further down
# (compile definitions + default assets generation).
if(CONFIG_BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW)
    # WZKJ ESP32-S3 AI Xiaowei Low (小未低配版)
    set(BOARD_DIR "wzkj/esp32-s3-ai-xiaowei_low")
elseif(CONFIG_BOARD_TYPE_WZKJ_ESP32_P4_AI_XIAOWEI_PLUS)
    # WZKJ ESP32-P4 AI Xiaowei Plus (小未 Plus / C61 无线协处理器)
    set(BOARD_DIR "wzkj/esp32-p4-ai-xiaowei_plus")
endif()

set(BUILTIN_TEXT_FONT font_noto_sans_basic_20_4)
set(BUILTIN_ICON_FONT font_material_symbols_20_4)
set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)


if(NOT BOARD_DIR AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(FATAL_ERROR "The selected board does not define BOARD_DIR")
endif()
list(APPEND INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}")

set(BOARD_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/config.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BOARD_CONFIG_FILE}")

# BOARD_TYPE and BOARD_MANUFACTURER are reported by the firmware.
# Prefer config.json and fall back to values derived from the full board directory.
set(BOARD_TYPE "")
set(BOARD_MANUFACTURER "")
if(EXISTS "${BOARD_CONFIG_FILE}")
    file(READ "${BOARD_CONFIG_FILE}" BOARD_CONFIG_JSON)
    string(REGEX MATCH "\"type\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_TYPE_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_TYPE_MATCH)
        set(BOARD_TYPE "${CMAKE_MATCH_1}")
    endif()
    string(REGEX MATCH "\"manufacturer\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" BOARD_MANUFACTURER_MATCH "${BOARD_CONFIG_JSON}")
    if(BOARD_MANUFACTURER_MATCH)
        set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT BOARD_TYPE)
    string(REPLACE "/" "-" BOARD_TYPE "${BOARD_DIR}")
endif()
if(NOT BOARD_MANUFACTURER AND BOARD_DIR MATCHES "^([^/]+)/")
    set(BOARD_MANUFACTURER "${CMAKE_MATCH_1}")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT BOARD_TYPE MATCHES "^[a-z0-9.-]+$")
    message(FATAL_ERROR
        "Invalid BOARD_TYPE '${BOARD_TYPE}': only lowercase letters, digits, '.' and '-' are allowed"
    )
endif()

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/*.c
)
# ⚠ 这个 glob 故意**不**加 CONFIGURE_DEPENDS：ESP-IDF 会用 script 模式重读本文件
# （component_get_requirements.cmake），而那个模式下 CONFIGURE_DEPENDS 是非法参数，
# 加了会让整个配置阶段失败（"CONFIGURE_DEPENDS is invalid for script and find
# package modes"）。
#
# 代价是新增的板级 .cc 不会被 glob 自动收进来 —— 编译照常通过、改动却静默不生效，
# 是个很难查的坑。所以新加的板级源文件必须在下面显式补一行。
#
# 但补的时候**必须挂在这块板自己的分支里**：文件只存在于那个板目录，无条件
# list(APPEND) 会让另一块板在配置阶段直接失败（Cannot find source file，
# 紧接着 No SOURCES given to target: __idf_main）。
if(CONFIG_BOARD_TYPE_WZKJ_ESP32_P4_AI_XIAOWEI_PLUS)
    # P4 板专属：AW88082 音频底层（S3 走 Es8311AudioCodec）。
    # 马达 / IMU 就是板目录里的 aw86937.cc、lsm6dsv80x.cc，跟着上面的 glob 走；
    # 对象由 xiaowei_plus.cc 持有，UI 通过它定义的 extern "C" 原语访问。
    list(APPEND BOARD_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/xiaowei_plus_audio.cc
    )
    # AW86937 uses the validated AWINIC driver and RAM waveform engine from lcd_demo.
    # 2026-10-14 合并：haptic_hv.c = 引擎核心 + aw8693x.c 芯片层；
    # haptic_hv_waveforms.c = 7 个 GUI 波形 + 1 个 RAM 启动表（原 haptic_hv_ram_custom.c）。
    # 头文件同理三合一：haptic_hv.h / haptic_hv_port.h / haptic_hv_waveforms.h。
    list(APPEND BOARD_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/aw86937/haptic_hv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/aw86937/haptic_hv_waveforms.c
    )
endif()

if(CONFIG_BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW)
    # S3 低配版：按键快捷菜单（low_ui.cc）自己也放在板目录里，但板级 glob 没有
    # CONFIGURE_DEPENDS，新文件必须显式列；三个图标原本只随 P4 的 UI 整段 glob
    # 编进去，S3 不参与那段，所以在这里单独补上。
    list(APPEND BOARD_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/low_ui.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ui_image_sound.c
        ${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ui_image_display.c
        ${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ui_image_weather_img.c
    )
    # display/src/ui 用来命中 lvgl/lvgl.h 的转发 shim（见 display/src/ui/lvgl/lvgl.h），
    # assets 用来命中 xiaowei_assets.h；P4 那段只加了 display/src/ui，S3 两个都要。
    list(APPEND INCLUDE_DIRS "display/src/ui" "display/src/ui/assets")
endif()
# 摄像头（GC2145 传感器 + DVP 采集管线）在板级目录的 gc2145.cc 里，跟着上面
# 的 BOARD_SOURCES glob 走 —— 它一直在板目录里，不需要单独补一行。原来这里
# 有过一个 if(CONFIG_BOARD_TYPE_..._PLUS) 块把根目录的 camera_test.cc 和
# boards/.../gc2145.cc 显式塞进 SOURCES，那个文件已删，gc2145.cc 那行是重复
# 追加（还绕过了下面这行去重，因为它直接进 SOURCES 而不是 BOARD_SOURCES）。
# 相机源文件按板目录隔离就够了，不必再加板型分支。

# build 目录重新生成时 glob 会把上面这个文件也捞进来，去重避免同一文件进 SOURCES 两次。
list(REMOVE_DUPLICATES BOARD_SOURCES)
list(APPEND SOURCES ${BOARD_SOURCES})

# Board-owned assets (haptic waveform images etc.), embedded only for the board
# that needs them. aw86937.cc references these by their linker symbol name
# (_binary_<filename with '.' -> '_'>_start), so a board that ships the driver
# must also ship the blobs -- otherwise the link fails on an undefined symbol.
# 目前为空：原来这里为 P4 板嵌了两个 AW86937 波形 blob
# （boards/.../vib_ramlib_heartbeat.bin、vib_wave_heartbeat.bin），但**这两个
# 文件不在仓库里**，而 EMBED_FILES 是硬依赖 —— 配置阶段就报
# "Cannot find source file"，连 build.ninja 都生成不出来。
#
# 而且它们是死的：全树没有任何代码引用 _binary_vib_ramlib_heartbeat_start /
# _binary_vib_wave_heartbeat_start。波形数据是以 C 数组形式编进来的 ——
# RAM 库是 aw86937/haptic_hv_ram_custom.c，GUI 波形是
# aw86937/haptic_hv_waveforms.c 的 aw86937_wave_alarm[]。
#
# 2026-10-14：那两个 .bin 连同三个从没参与过编译的备选源码
# （aw86937/haptic_hv_params.c、haptic_hv_params_old.c、
# haptic_hv_ram_heartbeat.c）已一并删除，共约 278 KB。对固件零影响 ——
# 它们既不在 BOARD_SOURCES 里，也不在 EMBED_FILES 里，连 .obj 都不会生成。
# 需要时从 lcd 工程取回：ui/lcd/main/boards/WZKJ/esp32-p4-ai-xiaowei_plus/。
#
# 如果以后确实要把 .bin 原样嵌进来（而不是用上面那几个 C 数组），把文件放进
# 板级目录再把 list(APPEND ...) 加回来即可；但那时要先确认真有代码按
# _binary_<name>_start 去取它，否则只是白白多占 flash。
set(BOARD_EMBED_FILES "")

# ---- 小未 LCD UI（仅 P4 板）-----------------------------------------------
# 这套 UI 整体来自 lcd 工程（display/src/ui），只编进 P4 板；S3 板完全不参与，
# 所以下面整段都锁在 P4 分支里 —— S3 的 SOURCES / 尺寸不受任何影响。
#
# 目录层级刻意与 donor 保持一致：UI 内部大量使用 "../framework/app_registry.h"
# 这类相对包含，以及 "lvgl/lvgl.h"；路径不变就不用改任何 include。
#
# 注意 donor 的 main.cpp / main_xiaowei.cpp（自建 LVGL 驱动层）**不在**这里 ——
# 那是它自己的 lv_init / flush_cb / lv_timer_handler，与本工程的 esp_lvgl_port
# 冲突，必须整段丢弃（见 display/xiaowei_ui_display.cc）。
if(CONFIG_BOARD_TYPE_WZKJ_ESP32_P4_AI_XIAOWEI_PLUS)
    file(GLOB_RECURSE XIAOWEI_UI_CORE
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/framework/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/shell/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/apps/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/platform/*.c")
    file(GLOB_RECURSE XIAOWEI_UI_CPP
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/*.cc")
    file(GLOB XIAOWEI_UI_RESOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ui_image_*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ui_font_*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ported/weather_assets/*.c")
    set(XIAOWEI_UI_EXTRA
        # 板级接线用的显示子类（InitializeLcd 里 new 的就是它）。
        # 位置在 display/ 而不是 display/src/ui/，所以不会被上面的 GLOB 捞到，显式列出。
        "${CMAKE_CURRENT_SOURCE_DIR}/display/xiaowei_ui_display.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/xiaowei_ui.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ported/weather_screen.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/display/src/ui/ported/simon_game.c")
    list(APPEND SOURCES ${XIAOWEI_UI_CORE} ${XIAOWEI_UI_CPP}
                        ${XIAOWEI_UI_RESOURCES} ${XIAOWEI_UI_EXTRA})
    list(APPEND INCLUDE_DIRS "display/src/ui")
endif()

# ESP32-S3 uses the AFE audio engine with the custom wake word
list(APPEND SOURCES "audio/engines/afe_audio_engine.cc")
list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
list(APPEND SOURCES "audio/wake_words/wake_word_audio_cache.cc")

# Select language directory according to Kconfig
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
elseif(CONFIG_LANGUAGE_KO_KR)
    set(LANG_DIR "ko-KR")
elseif(CONFIG_LANGUAGE_VI_VN)
    set(LANG_DIR "vi-VN")
elseif(CONFIG_LANGUAGE_TH_TH)
    set(LANG_DIR "th-TH")
elseif(CONFIG_LANGUAGE_DE_DE)
    set(LANG_DIR "de-DE")
elseif(CONFIG_LANGUAGE_FR_FR)
    set(LANG_DIR "fr-FR")
elseif(CONFIG_LANGUAGE_ES_ES)
    set(LANG_DIR "es-ES")
elseif(CONFIG_LANGUAGE_IT_IT)
    set(LANG_DIR "it-IT")
elseif(CONFIG_LANGUAGE_RU_RU)
    set(LANG_DIR "ru-RU")
elseif(CONFIG_LANGUAGE_AR_SA)
    set(LANG_DIR "ar-SA")
elseif(CONFIG_LANGUAGE_HI_IN)
    set(LANG_DIR "hi-IN")
elseif(CONFIG_LANGUAGE_PT_PT)
    set(LANG_DIR "pt-PT")
elseif(CONFIG_LANGUAGE_PL_PL)
    set(LANG_DIR "pl-PL")
elseif(CONFIG_LANGUAGE_CS_CZ)
    set(LANG_DIR "cs-CZ")
elseif(CONFIG_LANGUAGE_FI_FI)
    set(LANG_DIR "fi-FI")
elseif(CONFIG_LANGUAGE_TR_TR)
    set(LANG_DIR "tr-TR")
elseif(CONFIG_LANGUAGE_ID_ID)
    set(LANG_DIR "id-ID")
elseif(CONFIG_LANGUAGE_UK_UA)
    set(LANG_DIR "uk-UA")
elseif(CONFIG_LANGUAGE_RO_RO)
    set(LANG_DIR "ro-RO")
elseif(CONFIG_LANGUAGE_BG_BG)
    set(LANG_DIR "bg-BG")
elseif(CONFIG_LANGUAGE_CA_ES)
    set(LANG_DIR "ca-ES")
elseif(CONFIG_LANGUAGE_DA_DK)
    set(LANG_DIR "da-DK")
elseif(CONFIG_LANGUAGE_EL_GR)
    set(LANG_DIR "el-GR")
elseif(CONFIG_LANGUAGE_FA_IR)
    set(LANG_DIR "fa-IR")
elseif(CONFIG_LANGUAGE_FIL_PH)
    set(LANG_DIR "fil-PH")
elseif(CONFIG_LANGUAGE_HE_IL)
    set(LANG_DIR "he-IL")
elseif(CONFIG_LANGUAGE_HR_HR)
    set(LANG_DIR "hr-HR")
elseif(CONFIG_LANGUAGE_HU_HU)
    set(LANG_DIR "hu-HU")
elseif(CONFIG_LANGUAGE_MS_MY)
    set(LANG_DIR "ms-MY")
elseif(CONFIG_LANGUAGE_NB_NO)
    set(LANG_DIR "nb-NO")
elseif(CONFIG_LANGUAGE_NL_NL)
    set(LANG_DIR "nl-NL")
elseif(CONFIG_LANGUAGE_SK_SK)
    set(LANG_DIR "sk-SK")
elseif(CONFIG_LANGUAGE_SL_SI)
    set(LANG_DIR "sl-SI")
elseif(CONFIG_LANGUAGE_SV_SE)
    set(LANG_DIR "sv-SE")
elseif(CONFIG_LANGUAGE_SR_RS)
    set(LANG_DIR "sr-RS")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

# Collect current language audio files
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)

# If not en-US, collect en-US audio files as fallback for missing files
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg)

    # Extract filenames (without path) from current language
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()

    # Only add en-US audio files that are missing in current language
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
            message(STATUS "Using en-US fallback for missing audio: ${FILENAME}")
        endif()
    endforeach()
endif()

file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

set(MAIN_PRIV_REQUIRES_EXTRA "")
# 小未 LCD UI（仅 P4 板）用到的额外组件：
#   nvs_flash            —— 移植过来的 Simon Says 用它存最高分（namespace "simon"）
#   esp_emote_gen_player —— robot app（display/src/ui/apps/robot/robot_app.c）
#                           要 #include "emote_gen_player.h"
#
#  ⚠ 这里**必须**用 IDF_TARGET 门控，不能写 CONFIG_BOARD_TYPE_*。
#
#  原因：本工程开了 idf_build_set_property(MINIMAL_BUILD ON)，IDF 只保留
#  「main 的依赖闭包」里的组件。而这个闭包是用**早期展开**那一趟收到的
#  REQUIRES 算出来的，早期展开那趟 **CONFIG_* 全是空的** —— 实测同一个文件
#  在两趟里打印：
#      early=1  target=esp32p4  P4=[]     <- 早期展开，REQUIRES 就是这时收的
#      early=   target=esp32p4  P4=[y]    <- 正式构建
#  所以 if(CONFIG_BOARD_TYPE_...) 在早期展开里恒为假，esp_emote_gen_player
#  根本没进闭包 -> 正式构建时被整个剪掉 -> 它的 include 目录不存在 ->
#  robot_app.c 报 "emote_gen_player.h: No such file or directory"。
#
#  这个坑很隐蔽：源文件收集（本文件下面的 GLOB）不受影响，因为那是正式构建
#  那趟做的，那趟 CONFIG_* 是好的 —— 所以 UI 源码编得进去，只有 REQUIRES 丢。
#  同样写法的 nvs_flash 一直没暴露，是因为它本来就被别的组件传递拉进来了。
#
#  IDF_TARGET 在两趟里都有值，是这里唯一可靠的判据。本工程 P4 板 = esp32p4、
#  S3 板 = esp32s3，一一对应，语义与板型门控等价。
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND MAIN_PRIV_REQUIRES_EXTRA nvs_flash esp_emote_gen_player)
endif()

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS} ${BOARD_EMBED_FILES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_pm
                        esp_psram
                        esp_netif
                        esp_eth
                        esp_driver_gpio
                        esp_driver_uart
                        esp_driver_spi
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_ledc
                        esp_driver_cam
                        esp_driver_jpeg
                        esp_driver_ppa
                        esp_app_format
                        app_update
                        spi_flash
                        mbedtls
                        console
                        efuse
                        bt
                        fatfs
                        xiaozhi-fonts
                        ${MAIN_PRIV_REQUIRES_EXTRA}
                    )

# ============================================================================
#  P4 / S3 低配版：允许 qrcodegen 的重复定义
#
#  S3 低配版的按键菜单也用了 lv_qrcode，两条引用链和下面 P4 的情况完全一样，
#  所以同一个开关也挂到 S3 上。P4 之外的其它板不启用 lv_qrcode，不需要。
#
#  链接期报 13 个 "multiple definition of `qrcodegen_*'"，两份都真实存在、
#  都躲不掉，因为它们是**两条独立的引用链**：
#
#    lvgl__lvgl/lib...a(qrcodegen.c.obj)               <- 重复的一份
#    espressif2022__esp_emote_gfx/lib...a(qrcodegen.c.obj)  <- 先定义的一份
#
#    ① esp2022__esp_emote_expression 是 WHOLE_ARCHIVE 注册的，它的 emote_setup.c
#       / emote_op.c 引用 gfx_qrcode_create / gfx_qrcode_set_data
#         -> 提取 gfx_qrcode.o      -> 引用 qrcode_wrapper_*
#         -> 提取 qrcode_wrapper.o  -> 引用 qrcodegen_*
#         -> 提取【emote_gfx 自带的】qrcodegen.o
#    ② 本轮新接入的 UI 的 qrcode app（display/src/ui/apps/qrcode/qrcode_app.c）
#       调 lv_qrcode_* -> 提取 lv_qrcode.o -> 引用 qrcodegen_*
#         -> 提取【lvgl 自带的】qrcodegen.o
#
#  ⚠ 这个冲突是**接入 UI 才引入的**。改动前全工程没有任何地方调 lv_qrcode_*，
#  ① 那条链虽然一直在，但只有一份 qrcodegen 进链接，不冲突。
#
#  为什么允许重复是安全的（逐项核对过，不是想当然）：
#    1. 两份的公开头文件 ABI 完全一致：qrcodegen_VERSION_MAX 都是 40、
#       qrcodegen_BUFFER_LEN_FOR_VERSION 公式逐字相同、struct qrcodegen_Segment
#       布局相同、枚举值相同、13 个公开函数签名相同（唯一的文本差异是
#       `int mask` 与 `enum qrcodegen_Mask mask`，RISC-V ABI 下同为 4 字节整型）。
#       调用方按各自的头文件编译，落到另一份实现上仍是同一个二进制契约。
#    2. 两份的内部辅助函数全是 `testable`（= static）。nm 实测两个 .obj 的全局
#       符号分别只有 15 / 13 个，且**全部**是 qrcodegen_ 开头的公开 API ——
#       内部实现不会跨对象互绑，不存在"新壳套旧芯"的混搭。
#    3. 两份是同一上游库（Project Nayuki qrcodegen）的两个版本：lvgl 那份是旧版
#       内部命名 + LVGL 自加的 qrcodegen_getMinFitVersion / qrcodegen_version2size；
#       emote_gfx 那份是新版命名、**没有**这两个函数。所以链接器取先出现的
#       emote_gfx 那份满足 13 个共有符号，lvgl 那份仍必须进链接以提供它独有的
#       两个 —— 而这两个函数是自包含的（只调 lvgl 自己文件内的 static 辅助），
#       不会反向调用 emote_gfx 的实现。
#
#  为什么不做得"更干净"（从构建里删掉一份）：两份都在 managed_components/ 下，
#  想删掉任一份都得改它们自己的 CMakeLists（esp_emote_gfx 是
#  file(GLOB_RECURSE "src/*.c") 无差别收集）。改 vendor 目录一旦被组件管理器重新
#  解包就会被静默还原，复发时表现为"莫名其妙又编不过"。链接器开关完全在
#  我们的仓库里，可审计、可整段回退。
#
#  复发时怎么判断还是这个问题：抓 build 日志里的 "multiple definition of"，
#  应当**只有** qrcodegen 前缀的 13 个符号。若出现别的符号，那是另一处新冲突，
#  不要用这个开关糊过去。
# ============================================================================
if(IDF_TARGET STREQUAL "esp32p4" OR CONFIG_BOARD_TYPE_WZKJ_ESP32_S3_AI_XIAOWEI_LOW)
    idf_build_set_property(LINK_OPTIONS "-Wl,--allow-multiple-definition" APPEND)
endif()

# Use target_compile_definitions to define the reported board identity
# If BOARD_NAME is empty, use BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT BOARD_NAME MATCHES "^[a-z0-9.-]+$")
    message(FATAL_ERROR
        "Invalid BOARD_NAME '${BOARD_NAME}': only lowercase letters, digits, '.' and '-' are allowed"
    )
endif()
string(REGEX MATCH "^font_noto_sans_basic_([0-9]+)_([0-9]+)$" _font_match "${BUILTIN_TEXT_FONT}")
if(NOT _font_match)
    message(FATAL_ERROR "Unsupported built-in text font name: ${BUILTIN_TEXT_FONT}")
endif()
set(TEXT_FONT_SIZE "${CMAKE_MATCH_1}")
set(TEXT_FONT_BPP "${CMAKE_MATCH_2}")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB}
                        PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                        PRIVATE BOARD_MANUFACTURER=\"${BOARD_MANUFACTURER}\"
                        PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                        PRIVATE TEXT_FONT_SIZE=${TEXT_FONT_SIZE} TEXT_FONT_BPP=${TEXT_FONT_BPP}
                        )
endif()

# Add generation rules
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# Force build generation dependencies
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Find ESP-SR component dynamically
find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT ESP_SR_COMPONENT_PATH)
if(ESP_SR_COMPONENT_PATH)
    set(ESP_SR_MODEL_PATH "${ESP_SR_COMPONENT_PATH}/model")
endif()

# Find xiaozhi-fonts component dynamically
find_component_by_pattern("xiaozhi-fonts" NOTO_FONTS_COMPONENT NOTO_FONTS_COMPONENT_PATH)
if(NOTO_FONTS_COMPONENT_PATH)
    set(NOTO_FONTS_PATH "${NOTO_FONTS_COMPONENT_PATH}")
endif()

# Function to build default assets based on configuration
function(build_default_assets_bin)
    # Set output path for generated assets.bin
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    set(DEFAULT_ASSETS_DEPENDENCIES
        "${SDKCONFIG}"
        "${PROJECT_DIR}/scripts/build_default_assets.py"
    )

    # Prepare arguments for build script
    set(BUILD_ARGS
        "--sdkconfig" "${SDKCONFIG}"
        "--output" "${GENERATED_ASSETS_BIN}"
    )

    # Add builtin text font if defined
    if(BUILTIN_TEXT_FONT)
        list(APPEND BUILD_ARGS "--builtin_text_font" "${BUILTIN_TEXT_FONT}")
        string(REPLACE "_basic_" "_common_" COMMON_TEXT_FONT "${BUILTIN_TEXT_FONT}")
        set(COMMON_TEXT_FONT_FILE "${NOTO_FONTS_PATH}/cbin/${COMMON_TEXT_FONT}.bin")
        if(EXISTS "${COMMON_TEXT_FONT_FILE}")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${COMMON_TEXT_FONT_FILE}")
        endif()
        if(EXISTS "${NOTO_FONTS_PATH}/manifest.json")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${NOTO_FONTS_PATH}/manifest.json")
        endif()
    endif()

    # Add default emoji collection if defined
    if(DEFAULT_EMOJI_COLLECTION)
        list(APPEND BUILD_ARGS "--emoji_collection" "${DEFAULT_EMOJI_COLLECTION}")
        file(GLOB DEFAULT_EMOJI_FILES CONFIGURE_DEPENDS
            "${NOTO_FONTS_PATH}/png/${DEFAULT_EMOJI_COLLECTION}/*"
        )
        list(APPEND DEFAULT_ASSETS_DEPENDENCIES ${DEFAULT_EMOJI_FILES})
    endif()

    # Add default assets extra files if defined
    if(DEFAULT_ASSETS_EXTRA_FILES)
        list(APPEND BUILD_ARGS "--extra_files" "${DEFAULT_ASSETS_EXTRA_FILES}")
    endif()

    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--noto_fonts_path" "${NOTO_FONTS_PATH}")

    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS ${DEFAULT_ASSETS_DEPENDENCIES}
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )

    # Create target for generated assets
    add_custom_target(generated_default_assets ALL
        DEPENDS ${GENERATED_ASSETS_BIN}
    )

    # Set the generated file path in parent scope
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)

    message(STATUS "Default assets build configured: ${GENERATED_ASSETS_BIN}")
endfunction()


# ---- 外置 UI 大图（uiimg 分区）的容器 ----------------------------------------
#
# 与 build_default_assets_bin() 是两套独立的东西，刻意不合并：
#   assets 分区 → xiaozhi 的字体/表情/唤醒词，读端是 main/assets.cc 的 LvglStrategy
#   uiimg  分区 → LCD UI 的 3 张全屏壁纸，读端是
#                 display/src/ui/platform/xiaowei_ui_images.c
# 分开的理由见 partitions/16m_uiimg.csv 的注释 —— 核心是 assets 分区会被
# main/assets.cc 的 Download()/Apply() 在 OTA 下发新资源包时整块重写。
#
# 输入 display/external_assets/*.bin 是 scripts/externalize_ui_images.py 从那 8 个
# .c 里抽出来的（一次性转换，产物已入库；脚本留着是为了标明数据出处）。
function(build_ui_images_bin max_size)
    set(UI_IMG_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/display/external_assets")
    set(UI_IMG_BIN "${CMAKE_BINARY_DIR}/uiimg.bin")
    set(UI_IMG_INCLUDE_DIR "${CMAKE_BINARY_DIR}/uiimg_include")

    # CONFIGURE_DEPENDS：新增大图时不用手动 reconfigure
    file(GLOB UI_IMG_BLOBS CONFIGURE_DEPENDS "${UI_IMG_SRC_DIR}/*.bin")
    if(NOT UI_IMG_BLOBS)
        message(FATAL_ERROR
            "外置像素数据不存在: ${UI_IMG_SRC_DIR}/*.bin\n"
            "先执行: python scripts/externalize_ui_images.py")
    endif()

    add_custom_command(
        OUTPUT ${UI_IMG_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_ui_images_bin.py
                --input "${UI_IMG_SRC_DIR}"
                --output "${UI_IMG_BIN}"
                --include "${UI_IMG_INCLUDE_DIR}"
                --max-size "${max_size}"
        DEPENDS ${UI_IMG_BLOBS}
                "${PROJECT_DIR}/scripts/build_ui_images_bin.py"
                # build_ui_images_bin.py 直接 import 这个脚本里的 pack_assets_simple()，
                # 也就是说容器格式只有一处定义，它变了这里必须跟着重建。
                "${PROJECT_DIR}/scripts/build_default_assets.py"
        COMMENT "Building uiimg.bin (externalized LCD UI images)"
        VERBATIM)

    add_custom_target(generated_ui_images ALL DEPENDS ${UI_IMG_BIN})

    set(GENERATED_UI_IMAGES_LOCAL_FILE ${UI_IMG_BIN} PARENT_SCOPE)
    message(STATUS "uiimg container build configured: ${UI_IMG_BIN} (partition max ${max_size})")
endfunction()


# Function to get local assets file path (handles both URL and local file)
function(get_assets_local_file assets_source assets_local_file_var)
    # Check if it's a URL (starts with http:// or https://)
    if(assets_source MATCHES "^https?://")
        # It's a URL, download it
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        set(ASSETS_TEMP_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}.tmp")

        # Check if local file exists
        if(EXISTS ${ASSETS_LOCAL_FILE})
            message(STATUS "Assets file ${ASSETS_FILENAME} already exists, skipping download")
        else()
            message(STATUS "Downloading ${ASSETS_FILENAME}")

            # Clean up any existing temp file
            if(EXISTS ${ASSETS_TEMP_FILE})
                file(REMOVE ${ASSETS_TEMP_FILE})
            endif()

            # Download to temporary file first
            file(DOWNLOAD ${assets_source} ${ASSETS_TEMP_FILE}
                 STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Clean up temp file on failure
                if(EXISTS ${ASSETS_TEMP_FILE})
                    file(REMOVE ${ASSETS_TEMP_FILE})
                endif()
                message(FATAL_ERROR "Failed to download ${ASSETS_FILENAME} from ${assets_source}")
            endif()

            # Move temp file to final location (atomic operation)
            file(RENAME ${ASSETS_TEMP_FILE} ${ASSETS_LOCAL_FILE})
            message(STATUS "Successfully downloaded ${ASSETS_FILENAME}")
        endif()
    else()
        # It's a local file path
        if(IS_ABSOLUTE "${assets_source}")
            set(ASSETS_LOCAL_FILE "${assets_source}")
        else()
            set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
        endif()

        # Check if local file exists
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
        endif()

        message(STATUS "Using assets file: ${ASSETS_LOCAL_FILE}")
    endif()

    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()


partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if ("${size}" AND "${offset}")
    # Flash assets based on configuration
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        # Build default assets based on configuration
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
        message(STATUS "Generated default assets flash configured: ${GENERATED_ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        # Flash custom assets
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
        message(STATUS "Custom assets flash configured: ${ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_NONE_ASSETS)
        message(STATUS "Assets flashing disabled (FLASH_NONE_ASSETS)")
    endif()
else()
    message(STATUS "Assets partition not found, using v1 partition table")
endif()


# ---- 外置 UI 大图 + emote 表情包（仅 P4 板）--------------------------------
#
# 两块分区都只存在于 P4 专用的 partitions/16m_uiimg.csv 里（S3 走 16m.csv），
# 所以整段锁在 P4 分支里。分区找不到就直接 FATAL_ERROR —— 这张表是这套外置
# 方案的前提，静默跳过只会换来实机上一片黑屏（甚至空指针），不如构建期就炸。
if(CONFIG_BOARD_TYPE_WZKJ_ESP32_P4_AI_XIAOWEI_PLUS)
    # uiimg：3 张全屏壁纸的 mmap 容器（原先还放着 5 张姿态仪大图，IMU 测试页
    # 改成纯滑条之后那 5 张已经删掉，分区尺寸没动，只是余量更大了）
    partition_table_get_partition_info(UI_IMG_PARTITION_SIZE "--partition-name uiimg" "size")
    if(NOT "${UI_IMG_PARTITION_SIZE}")
        message(FATAL_ERROR
            "分区表里没有 uiimg 分区。P4 板必须用 partitions/16m_uiimg.csv"
            "（见 sdkconfig.defaults.esp32p4）。")
    endif()
    build_ui_images_bin("${UI_IMG_PARTITION_SIZE}")
    esptool_py_flash_to_partition(flash "uiimg" "${GENERATED_UI_IMAGES_LOCAL_FILE}")
    message(STATUS "Externalized UI images flash configured: "
                   "${GENERATED_UI_IMAGES_LOCAL_FILE} -> uiimg (${UI_IMG_PARTITION_SIZE} B)")

    # emote：robot 页的表情包。这是 donor 直接烧进分区的原始 mmap 容器
    # （esp_mmap_assets 格式，含 index.json），不需要任何转换 —— 所以不像上面
    # 两个那样有生成步骤，直接烧。消费者是 components/esp_emote_gen_player。
    partition_table_get_partition_info(EMOTE_PARTITION_SIZE "--partition-name emote" "size")
    if(NOT "${EMOTE_PARTITION_SIZE}")
        message(FATAL_ERROR
            "分区表里没有 emote 分区。P4 板必须用 partitions/16m_uiimg.csv"
            "（见 sdkconfig.defaults.esp32p4）。")
    endif()
    set(EMOTE_ASSETS_BIN "${CMAKE_CURRENT_SOURCE_DIR}/display/Expression/emote-assets.bin")
    if(NOT EXISTS "${EMOTE_ASSETS_BIN}")
        message(FATAL_ERROR "表情包文件不存在: ${EMOTE_ASSETS_BIN}")
    endif()
    file(SIZE "${EMOTE_ASSETS_BIN}" EMOTE_ASSETS_SIZE)
    if(EMOTE_ASSETS_SIZE GREATER "${EMOTE_PARTITION_SIZE}")
        message(FATAL_ERROR
            "表情包 ${EMOTE_ASSETS_SIZE} B 超过 emote 分区 ${EMOTE_PARTITION_SIZE} B。")
    endif()
    esptool_py_flash_to_partition(flash "emote" "${EMOTE_ASSETS_BIN}")
    message(STATUS "Emote assets flash configured: "
                   "${EMOTE_ASSETS_BIN} -> emote (${EMOTE_ASSETS_SIZE}/${EMOTE_PARTITION_SIZE} B)")
endif()
