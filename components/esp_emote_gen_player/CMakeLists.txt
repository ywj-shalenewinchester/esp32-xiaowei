set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖用 REQUIRES 声明，**不要**用 idf_component.yml。
#
# 这个组件是从 lcd 工程本地拷过来的，donor 的 idf_component.yml 里写着
# espressif2022/esp_emote_gfx ^3.0.0 + espressif/esp_mmap_assets *。目标工程的
# managed_components/ 里这两个**早就装好了**（由 main/idf_component.yml 的
# espressif2022/esp_emote_expression 传递拉进来，版本已记在 dependencies.lock），
# 所以本地依赖是满足的。
#
# 但组件管理器判断"是否需要重新求解"看的是**清单文件集合**：新增一个带
# idf_component.yml 的本地组件 = 清单变了 -> 触发全量重新求解 -> 必须联网问
# components-file.espressif.com 要每个依赖的元数据。离线就直接炸：
#
#   ERROR: Cannot establish a connection to the component registry.
#   URL: https://components-file.espressif.com/components/78/esp-ml307.json
#
# 去掉清单后，这个组件就是一个普通的本地组件（不参与依赖求解），而它需要的
# 头文件靠下面的 REQUIRES 拿 —— 效果完全一样，且全程离线可编。
#
# 各依赖对应的头文件（实测）：
#   json            -> cJSON.h                  (emote_gen_player_assets.c)
#   esp_mmap_assets -> esp_mmap_assets.h        (assets.c / player.c)
#   esp_emote_gfx   -> gfx.h, core/gfx_disp.h,
#                      widget/gfx_anim.h, widget/gfx_label.h
#   lvgl            -> lvgl.h
set(COMPONENT_REQUIRES json esp_mmap_assets esp_emote_gfx lvgl)

# ============================================================================
#  只为 P4 板编译
#
#  这个组件是 lcd 工程里的本地组件，被移植过来的 robot app
#  (main/display/src/ui/apps/robot/robot_app.c) 使用。
#
#  那套 UI 只跑在 P4 板（wzkj/esp32-p4-ai-xiaowei_plus）上，所以这个组件也只该
#  编进 P4 —— 否则 S3 板会白白多出 734 KB 源文件（其中字体 font_puhui_basic_20_4.c
#  就占 699 KB），违背"S3 完全不碰"的约束。
#
#  这里用「空注册」而不是干脆不建这个目录：组件目录一旦存在，IDF 就会尝试构建它，
#  空注册 + return 是让它在非 P4 目标下变成一个无源码、无产物的空组件的标准做法。
#
#  ⚠ 判据用 IDF_TARGET，**不要**写 CONFIG_BOARD_TYPE_*。
#  本工程开了 MINIMAL_BUILD，IDF 的「早期展开」那一趟里 CONFIG_* 全是空的
#  （实测 early=1 时 P4=[]、正式构建时 P4=[y]），用 CONFIG_* 会让这个 if 在
#  早期展开里恒真、直接空注册；更糟的是 main/CMakeLists.txt 那边同样用
#  CONFIG_* 门控 REQUIRES 时，组件会被整个剪出依赖闭包。
#  IDF_TARGET 两趟都有值，是唯一可靠判据。详见 main/CMakeLists.txt 里的长注释。
# ============================================================================
if(NOT IDF_TARGET STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS
        "src/emote_gen_player.c"
        "src/emote_gen_player_assets.c"
        "src/fonts/font_puhui_basic_20_4.c"
    INCLUDE_DIRS "include"
    REQUIRES ${COMPONENT_REQUIRES}
)
