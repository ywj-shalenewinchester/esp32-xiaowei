#!/usr/bin/env python3
"""
把 LCD UI 里的大图从 .c 源码内嵌字节数组改成「运行期绑定」。

背景
----
接入 LCD UI 后 P4 固件到了 6.74 MB，而双 OTA 布局下 app 槽只有 5 MB。
占地方的就是 8 张全屏级大图（3 张 466x466 壁纸 + 5 张姿态仪图，共约 2.2 MB）。
另外 robot 页要用的 emote 表情包（1.73 MB）完全没地方放。
（那 5 张姿态仪图后来随 IMU 测试页重做删掉了，现在只剩 3 张壁纸，共约 1.27 MB。）

做法：把这些图的像素数据抽成 .bin，打进独立的 uiimg 分区（mmap 容器，
由 scripts/build_ui_images_bin.py 生成），运行期再填回描述符的 .data。
描述符本身留在源码里（尺寸/颜色格式这些元数据要编译期可见），只是变成
**非 const** 且 .data = NULL，等 xiaowei_ui_bind_external_assets() 来填。

这个脚本做两件事
----------------
1. 从 TARGETS 列的 .c 里抽出像素字节 -> main/display/external_assets/*.bin
2. 原地改写这些 .c：
     - 删掉内嵌数组
     - 去掉描述符的 const（否则运行期没法赋值）
     - .data_size 从 sizeof(数组) 换成字面量
     - .data 换成 NULL

⚠ 只能对「还没抽过」的原始文件跑。数组已经不在了就会报错退出，
   所以别指望用它反向还原 —— .bin 才是数据本体，请一并纳入版本管理。

用法
----
    python scripts/externalize_ui_images.py [--dry-run]
"""

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# 待外置的 3 张图。blob 文件名就是运行期查表的 key，改名要同步改
# main/display/src/ui/platform/xiaowei_ui_images.c 里的 s_bindings 绑定表。
# 名字长度必须 <= 32（容器的 max_name_len，见 pack_assets_simple）。
#
# 曾经还有 5 张姿态仪图（dial_bg / pitch_scale / roll_scale / roll_scale2 /
# pointer）。IMU 测试页从"大图旋转的姿态仪"改成纯滑条之后，那 5 张连同
# 它们的外置 .bin 一起删了，这里也就只剩壁纸。
# ---------------------------------------------------------------------------

UI_DIR = os.path.join("main", "display", "src", "ui")
BLOB_DIR = os.path.join("main", "display", "external_assets")

TARGETS = [
    # (源文件, blob 文件名)
    (os.path.join(UI_DIR, "ui_image_wallpaper_img1.c"), "wallpaper_img1.bin"),
    (os.path.join(UI_DIR, "ui_image_wallpaper_img2.c"), "wallpaper_img2.bin"),
    (os.path.join(UI_DIR, "ui_image_wallpaper_img3.c"), "wallpaper_img3.bin"),
]

# 描述符声明：
#   const lv_image_dsc_t img_wallpaper_img1 = {      （壁纸；LVGL9 里 lv_img_dsc_t 是别名）
DSC_RE = re.compile(
    r"(?m)^(?P<indent>[ \t]*)"
    r"(?P<qual>const[ \t]+)?"
    r"(?P<type>lv_image_dsc_t|lv_img_dsc_t)[ \t]+"
    r"(?P<name>\w+)[ \t]*=[ \t]*\{"
)

BLANK_LINE_RE = re.compile(r"\n[ \t]*\n")

# 运行时绑定的说明块，插在被删掉的数组位置上。
REPLACEMENT_COMMENT = """\
/* 像素数据已外置到 uiimg 分区（由 scripts/build_ui_images_bin.py 打成 mmap 容器），
 * 运行期由 xiaowei_ui_bind_external_assets() 填进上面的 .data。
 * 别把数据搬回这个文件：它进的是 app 槽（5 MB），而 app 槽已经放不下了。 */
"""


def find_dsc(text, path):
    """定位描述符语句，返回 (match, block_start, block_end, block_text)。"""
    m = DSC_RE.search(text)
    if m is None:
        raise SystemExit(f"{path}: 没找到 lv_image_dsc_t / lv_img_dsc_t 描述符")
    block_start = m.start()
    brace = text.index("{", m.end() - 1)
    close = text.index("}", brace)  # 描述符里没有嵌套花括号
    close = text.index(";", close) + 1
    return m, block_start, close, text[block_start:close]


def extract_pixels(text, dsc_block, path):
    """按描述符里的 .data = <数组名> 反查数组，抽出像素字节。"""
    m = re.search(r"\.data[ \t]*=[ \t]*(\w+)", dsc_block)
    if m is None:
        raise SystemExit(f"{path}: 描述符里没有 .data = <数组> —— 是不是已经抽过了？")
    array_name = m.group(1)

    arr = re.search(
        r"(?m)^[^\n]*\b" + re.escape(array_name) + r"[ \t]*\[[ \t]*\][ \t]*=[ \t]*\{",
        text,
    )
    if arr is None:
        raise SystemExit(f"{path}: 找不到数组 {array_name} 的定义 —— 是不是已经抽过了？")

    # 数组语句可能跨行（壁纸是 static const / 属性宏 / 类型名 三行），
    # 往前退到上一个空行，把整个声明一起删掉。
    prev = None
    for b in BLANK_LINE_RE.finditer(text, 0, arr.start()):
        prev = b
    stmt_start = prev.end() if prev else 0

    brace = text.index("{", arr.end() - 1)
    close_brace = text.index("}", brace)
    stmt_end = text.index(";", close_brace) + 1

    body = text[brace + 1 : close_brace]
    tokens = re.findall(r"0[xX]([0-9a-fA-F]+)", body)
    if not tokens:
        raise SystemExit(f"{path}: 数组 {array_name} 里没找到任何十六进制字节")

    data = bytearray()
    for t in tokens:
        if len(t) > 2:
            raise SystemExit(f"{path}: 数组 {array_name} 里出现超过两位的字节 0x{t}")
        data.append(int(t, 16))

    return array_name, bytes(data), stmt_start, stmt_end


def rewrite_dsc(dsc_block, name, type_name, qual, indent, data_size):
    """去掉 const、把 sizeof 换成字面量、把 .data 换成 NULL。"""
    out = dsc_block

    if qual:
        # 只去掉 "const " 这个限定符，保留缩进与其余内容
        out = re.sub(
            r"(?m)^(?P<i>[ \t]*)const[ \t]+(?P<t>" + type_name + r")\b",
            r"\g<i>\g<t>",
            out,
            count=1,
        )

    new_size, n_size = re.subn(
        r"\.data_size[ \t]*=[ \t]*sizeof[ \t]*\([^)]*\)",
        f".data_size = {data_size}",
        out,
    )
    if n_size != 1:
        raise SystemExit(f"{name}: 期望 1 处 .data_size = sizeof(...)，实际 {n_size} 处")

    new_data, n_data = re.subn(r"\.data[ \t]*=[ \t]*\w+", ".data = NULL", new_size)
    if n_data != 1:
        raise SystemExit(f"{name}: 期望 1 处 .data = <数组>，实际 {n_data} 处")

    return new_data


def process(path, blob_name, dry_run):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    m, block_start, block_end, dsc_block = find_dsc(text, path)
    array_name, pixels, stmt_start, stmt_end = extract_pixels(text, dsc_block, path)

    print(
        f"  {os.path.basename(path):32s} {array_name:28s} "
        f"{len(pixels):>9,d} B -> {blob_name}"
    )

    if dry_run:
        return len(pixels)

    # 先替换描述符（在数组语句之前，两者不重叠），再删数组语句。
    new_block = rewrite_dsc(
        dsc_block, m.group("name"), m.group("type"), m.group("qual"),
        m.group("indent"), len(pixels),
    )
    text = text[:block_start] + new_block + text[block_end:]
    text = text[:stmt_start] + REPLACEMENT_COMMENT + text[stmt_end:]

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)

    os.makedirs(BLOB_DIR, exist_ok=True)
    with open(os.path.join(BLOB_DIR, blob_name), "wb") as f:
        f.write(pixels)

    return len(pixels)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--dry-run", action="store_true", help="只统计，不改文件")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    print(f"工程根目录: {root}")
    print(f"blob 输出到: {BLOB_DIR}\n")

    total = 0
    for path, blob_name in TARGETS:
        if not os.path.isfile(path):
            raise SystemExit(f"找不到源文件: {path}")
        total += process(path, blob_name, args.dry_run)

    print(f"\n合计 {len(TARGETS)} 张图，{total:,d} B ({total / 1024:.1f} KB)")
    if args.dry_run:
        print("（--dry-run，没有落盘）")
    else:
        print("下一步：idf.py reconfigure && idf.py build")


if __name__ == "__main__":
    main()
