#!/usr/bin/env python3
"""
把外置的 UI 大图打成 uiimg 分区用的 mmap 容器。

容器格式不在这里定义 —— 直接复用 scripts/build_default_assets.py 里的
pack_assets_simple()，也就是 xiaozhi 自己的 assets 分区用的同一套格式
（表项 + "ZZ" 魔数 + 负载，头部带文件数/校验和/长度）。
这样读取端两边都能用同一批库代码：
  - xiaozhi 的资源读 main/assets.cc 的 LvglStrategy
  - 本工程的 UI 大图读 esp_mmap_assets（见 xiaowei_assets_bind.cc）

为什么单开一个分区而不是塞进 assets：
  1) 运行期 main/assets.cc 的 Download()/Apply() 会**整块重写** assets 分区
     （OTA 下发新资源包时），塞进去的图会被无声抹掉；
  2) assets 分区的字节布局保持不动，xiaozhi 的字体/表情/唤醒词路径零回归风险；
  3) 尺寸上不吃亏：uiimg 单开之后 app 槽仍是 5 MB。
  详见 partitions/16m_uiimg.csv 的注释。

用法（由 main/CMakeLists.txt 的 build_ui_images_bin() 调用）
    python scripts/build_ui_images_bin.py \
        --input main/display/external_assets \
        --output build/uiimg.bin \
        --include build/uiimg_include \
        --max-size 0x240000
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_default_assets import pack_assets_simple  # noqa: E402

# 容器的 max_name_len。与 assets 那个容器保持一致（32）。
# ⚠ 表项里的文件名就是运行期查表的 key，改这个值不影响查表，
#   但会让容器格式与 assets 分区的不一致，没有必要。
NAME_LENGTH = 32


def main():
    ap = argparse.ArgumentParser(description="Build the uiimg mmap container")
    ap.add_argument("--input", required=True, help="外置像素数据目录（*.bin）")
    ap.add_argument("--output", required=True, help="输出的容器 .bin 路径")
    ap.add_argument("--include", required=True, help="生成的 mmap_generate_uiimg.h 放哪")
    ap.add_argument(
        "--max-size",
        default=None,
        help="分区容量上限（支持 0x 前缀）。超过就直接失败，"
             "免得等到 esptool 烧录时才报 partition too small。",
    )
    args = ap.parse_args()

    if not os.path.isdir(args.input):
        sys.exit(f"输入目录不存在: {args.input}\n"
                 f"先跑 scripts/externalize_ui_images.py 生成外置像素数据。")

    bins = sorted(f for f in os.listdir(args.input) if f.endswith(".bin"))
    if not bins:
        sys.exit(f"{args.input} 里没有 .bin —— 先跑 scripts/externalize_ui_images.py")
    print(f"uiimg: 打包 {len(bins)} 个文件: {', '.join(bins)}")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    os.makedirs(args.include, exist_ok=True)
    pack_assets_simple(args.input, args.include, args.output, "uiimg", NAME_LENGTH)

    size = os.path.getsize(args.output)
    print(f"uiimg: {args.output} = {size:,d} B ({size / 1024:.1f} KB)")

    if args.max_size is not None:
        limit = int(args.max_size, 0)
        if size > limit:
            sys.exit(
                f"uiimg 容器 {size:,d} B 超过分区容量 {limit:,d} B "
                f"(超 {size - limit:,d} B)。请调整 partitions/16m_uiimg.csv 的 uiimg 分区大小。"
            )
        print(f"uiimg: 分区容量 {limit:,d} B，余量 {limit - size:,d} B")


if __name__ == "__main__":
    main()
