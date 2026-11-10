# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# 校正 ESP_IDF_VERSION，必须在 project() 之前
#
# 背景：esp_wifi_remote 的 Kconfig 第 10 行是
#     orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
# 它拼的是**文件名**，而这个文件只按「主.次」命名（Kconfig.idf_v5.5.in）；
# IDF 自己另外那个 IDF_VERSION 是「主.次.补丁」（config.env 里写的是 "5.5.4"）。
#
# 这两个一旦搞混，后果很隐蔽：orsource 是「可选 source」，文件找不到时**不报错、
# 静默跳过**，于是 esp_hosted / esp_wifi_remote 整棵 Kconfig 子树（SLAVE_IDF_TARGET、
# ESP_HOSTED_CP_TARGET_* 、WIFI_RMT_* 等）全都不存在 —— sdkconfig.defaults.esp32p4 里
# 对应的那些行会被当成未知符号丢掉，slave target 退回 esp_hosted 的默认 esp32h2/SPI。
# 但注入头文件 idf_v5.5/include/injected/esp_wifi.h 照旧在 include path 上
# （CMake 那边用的是 IDF_VERSION_MAJOR.MINOR，跟这个环境变量无关），
# 于是 WIFI_INIT_CONFIG_DEFAULT() 里的 CONFIG_WIFI_RMT_* 全部 undeclared，编译失败。
#
# 实测：ESP_IDF_VERSION=5.5 → SLAVE_IDF_TARGET=esp32c61、sdkconfig.h 里 37 条 WIFI_RMT；
#       ESP_IDF_VERSION=5.5.4 → 退回 esp32h2、sdkconfig.h 里 0 条 WIFI_RMT（即上述报错）。
#
# 所以这里不依赖外部 shell（export.ps1 给的是对的主.次，但 IDE 终端、自建启动脚本
# 可能塞进完整版本号），直接从 IDF 自己的 version.cmake 取主.次覆盖掉。
# 注意：整个 IDF 构建里 ESP_IDF_VERSION 的唯一消费者就是上面那行 orsource，覆盖是安全的。
# ---------------------------------------------------------------------------
include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.4.2")
project(xiaozhi)
