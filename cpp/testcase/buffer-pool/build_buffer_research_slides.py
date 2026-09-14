#!/usr/bin/env python3
"""Generate a compact four-slide PPTX for the buffer research overview.

The presentation uses full-slide PNGs inside a standard OOXML package. This keeps
Chinese fonts and scientific figures stable without requiring python-pptx.
"""

from __future__ import annotations

import argparse
import zipfile
from pathlib import Path

import matplotlib.font_manager as fm
import matplotlib.image as mpimg
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle


SLIDE_W = 13.333
SLIDE_H = 7.5
EMU_W = 12192000
EMU_H = 6858000
NAVY = "#17365D"
BLUE = "#2878B5"
TEAL = "#2A9D8F"
ORANGE = "#D95F02"
PURPLE = "#6C5CE7"
GRAY = "#5B6573"
LIGHT = "#F3F6F9"


def configure_font():
    bundled_font = Path("/home/whz/.local/share/fonts/NotoSansCJK-Regular.ttc")
    if bundled_font.is_file():
        fm.fontManager.addfont(bundled_font)
        plt.rcParams["font.family"] = fm.FontProperties(fname=bundled_font).get_name()
        plt.rcParams["axes.unicode_minus"] = False
        return
    candidates = [
        "Noto Sans CJK JP", "Noto Sans CJK SC", "Source Han Sans SC", "Microsoft YaHei",
        "WenQuanYi Micro Hei", "Droid Sans Fallback",
    ]
    available = {font.name for font in fm.fontManager.ttflist}
    for candidate in candidates:
        if candidate in available:
            plt.rcParams["font.family"] = candidate
            break
    plt.rcParams["axes.unicode_minus"] = False


def new_slide(title: str, number: int):
    fig = plt.figure(figsize=(SLIDE_W, SLIDE_H), dpi=150, facecolor="white")
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")
    if title:
        ax.text(0.055, 0.925, title, fontsize=25, weight="bold", color=NAVY,
                ha="left", va="center")
        ax.plot([0.055, 0.945], [0.875, 0.875], color="#D8E1EA", linewidth=1.5)
    ax.text(0.95, 0.035, str(number), fontsize=9, color="#8995A3",
            ha="right", va="center")
    return fig, ax


def save_slide(fig, path: Path):
    fig.savefig(path, dpi=150, facecolor="white")
    plt.close(fig)


def box(ax, xy, width, height, text, facecolor, edgecolor=NAVY, fontsize=14):
    patch = FancyBboxPatch(
        xy, width, height, boxstyle="round,pad=0.015,rounding_size=0.025",
        facecolor=facecolor, edgecolor=edgecolor, linewidth=1.5,
    )
    ax.add_patch(patch)
    ax.text(xy[0] + width / 2, xy[1] + height / 2, text,
            fontsize=fontsize, ha="center", va="center", color="#263442")
    return patch


def arrow(ax, start, end, color=NAVY):
    ax.add_patch(FancyArrowPatch(start, end, arrowstyle="-|>", mutation_scale=18,
                                linewidth=2, color=color))


def slide_title(output: Path):
    fig, ax = new_slide("", 1)
    ax.add_patch(Rectangle((0, 0), 1, 1, facecolor="#F7F9FC", edgecolor="none"))
    ax.add_patch(Rectangle((0, 0), 0.018, 1, facecolor=NAVY, edgecolor="none"))
    ax.text(0.07, 0.76, "Double Buffer 与选择性 Buffer 扩容",
            fontsize=31, weight="bold", color=NAVY, ha="left")
    ax.text(0.07, 0.68, "在保留 I/O–计算并行的同时降低内存开销",
            fontsize=19, color=GRAY, ha="left")

    box(ax, (0.08, 0.36), 0.34, 0.17,
        "Double Buffer\n重叠 I/O 与解码/计算", "#EAF2F8", fontsize=17)
    arrow(ax, (0.44, 0.445), (0.56, 0.445), color=TEAL)
    box(ax, (0.58, 0.36), 0.34, 0.17,
        "Selective Dynamic\n复用已有容量，按需扩容", "#E8F8F5", fontsize=17)

    ax.text(0.08, 0.23, "研究主线", fontsize=12, weight="bold", color=TEAL)
    ax.text(0.08, 0.17,
            "先证明双缓冲有效，再解决每线程两套 Buffer 带来的内存问题",
            fontsize=16, color="#263442")
    ax.text(0.92, 0.08,
            "证据覆盖 Pixels/io_uring · Pixels/SPDK · Parquet/io_uring",
            fontsize=11, color=GRAY, ha="right")
    save_slide(fig, output)


def slide_backend(output: Path, figure_path: Path):
    fig, ax = new_slide("1  Double Buffer 的收益不依赖特定异步后端", 2)
    image = mpimg.imread(figure_path)
    image_ax = fig.add_axes([0.055, 0.15, 0.67, 0.68])
    image_ax.imshow(image)
    image_ax.axis("off")

    ax.text(0.76, 0.78, "核心数据", fontsize=15, weight="bold", color=NAVY)
    points = [
        ("SPDK", "44 查询均值：1.178× → 1.046×", ORANGE),
        ("io_uring", "44 查询均值：1.140× → 1.053×", BLUE),
        ("Parquet", "q24：1.212× / 1.215× / 1.056×", TEAL),
    ]
    y = 0.68
    for name, value, color in points:
        ax.add_patch(Rectangle((0.76, y - 0.012), 0.012, 0.012,
                               facecolor=color, edgecolor="none"))
        ax.text(0.78, y, name, fontsize=13, weight="bold", color=color, va="center")
        ax.text(0.78, y - 0.055, value, fontsize=11, color="#263442", va="center")
        y -= 0.16

    ax.add_patch(FancyBboxPatch((0.75, 0.15), 0.20, 0.16,
                                boxstyle="round,pad=0.015,rounding_size=0.02",
                                facecolor="#FFF4D6", edgecolor="#E0B44C", linewidth=1.2))
    ax.text(0.85, 0.245, "结论", fontsize=12, weight="bold", color="#8A5A00", ha="center")
    ax.text(0.85, 0.19, "收益来自异步流水线重叠\n而不是某个后端的特例",
            fontsize=12, color="#5D4700", ha="center", va="center")
    save_slide(fig, output)


def slide_raw_io(output: Path):
    fig, ax = new_slide("2  Raw Physical I/O Scan：测量平台吞吐上限", 3)

    chart = fig.add_axes([0.075, 0.20, 0.53, 0.58])
    threads = [12, 24, 48]
    series = {
        "non-fixed": ([66.18, 109.57, 109.87], GRAY, "o"),
        "dynamic": ([68.80, 109.57, 110.14], BLUE, "s"),
        "static": ([68.74, 109.70, 110.34], PURPLE, "^"),
    }
    for label, (values, color, marker) in series.items():
        chart.plot(threads, values, marker=marker, linewidth=2.4, markersize=7,
                   color=color, label=label)
    chart.set_xlabel("Worker threads", fontsize=11)
    chart.set_ylabel("Aggregate read throughput (GB/s)", fontsize=11)
    chart.set_xticks(threads)
    chart.set_ylim(55, 116)
    chart.grid(axis="y", alpha=0.22)
    chart.legend(frameon=False, ncol=3, loc="lower right", fontsize=9)
    chart.set_title("24 SSD · 1 MiB request · queue depth 32", fontsize=12, weight="bold")

    scale = fig.add_axes([0.68, 0.48, 0.27, 0.28])
    ssds = [1, 4, 8, 12, 24]
    scaling = [6.14, 23.47, 44.07, 54.18, 109.12]
    scale.plot(ssds, scaling, marker="o", linewidth=2.2, color=TEAL)
    scale.set_title("Device scaling", fontsize=11, weight="bold")
    scale.set_xlabel("SSDs", fontsize=9)
    scale.set_ylabel("GB/s", fontsize=9)
    scale.set_xticks(ssds)
    scale.grid(alpha=0.2)

    ax.text(0.68, 0.39, "观测上限", fontsize=13, weight="bold", color=NAVY)
    ax.text(0.68, 0.31, "110.34 GB/s", fontsize=27, weight="bold", color=TEAL)
    ax.text(0.68, 0.245, "≈ 105K IOPS（1 MiB 请求）", fontsize=13, color="#263442")
    ax.text(0.68, 0.175, "24线程后进入平台期", fontsize=12, color=GRAY)
    ax.text(0.68, 0.115, "fixed-buffer 未饱和时约快 4%", fontsize=12, color=GRAY)
    ax.text(0.055, 0.07,
            "Timed region 不含文件发现及 ring/buffer 初始化；每组合预热1次、正式运行3次，中位数。",
            fontsize=10.5, color=GRAY)
    save_slide(fig, output)


def slide_doublebuffer_io(output: Path):
    fig, ax = new_slide("3  Double Buffer 的 IOPS 与读取带宽", 4)
    threads = [12, 24, 48]

    spdk = fig.add_axes([0.07, 0.25, 0.40, 0.52])
    spdk_single_bw = [12630.821352 / 1024, 24765.474207 / 1024, 36485.135256 / 1024]
    spdk_double_bw = [15376.406689 / 1024, 28728.975440 / 1024, 40033.051565 / 1024]
    spdk_double_iops = [13904, 25978, 36200]
    spdk.plot(threads, spdk_single_bw, marker="o", linewidth=2.2,
              color="#A0A8B0", label="single")
    spdk.plot(threads, spdk_double_bw, marker="o", linewidth=2.5,
              color=ORANGE, label="double")
    for x, y, iops in zip(threads, spdk_double_bw, spdk_double_iops):
        spdk.annotate(f"{iops / 1000:.1f}K IOPS", (x, y), xytext=(0, 9),
                      textcoords="offset points", ha="center", fontsize=8.5, color=ORANGE)
    spdk.set_title("Pixels / SPDK · internal completions", fontsize=12, weight="bold")
    spdk.set_xlabel("Worker threads", fontsize=10)
    spdk.set_ylabel("Read bandwidth (GiB/s)", fontsize=10)
    spdk.set_xticks(threads)
    spdk.set_ylim(8, 44)
    spdk.grid(axis="y", alpha=0.22)
    spdk.legend(frameon=False, fontsize=9)

    iouring = fig.add_axes([0.54, 0.25, 0.40, 0.52])
    io_bw = [12968.010449 / 1024, 20997.470313 / 1024, 26487.896777 / 1024]
    io_iops = [35055, 56760, 71602]
    iouring.plot(threads, io_bw, marker="o", linewidth=2.5, markersize=7,
                 color=BLUE, label="double buffer")
    for x, y, iops in zip(threads, io_bw, io_iops):
        iouring.annotate(f"{iops / 1000:.1f}K IOPS", (x, y), xytext=(0, 10),
                         textcoords="offset points", ha="center", fontsize=8.5, color=BLUE)
    iouring.set_title("Pixels / io_uring · device iostat", fontsize=12, weight="bold")
    iouring.set_xlabel("Worker threads", fontsize=10)
    iouring.set_ylabel("Device read bandwidth (GiB/s)", fontsize=10)
    iouring.set_xticks(threads)
    iouring.set_ylim(8, 29)
    iouring.grid(axis="y", alpha=0.22)
    iouring.legend(frameon=False, fontsize=9)

    ax.text(0.07, 0.16,
            "SPDK q24：Double Buffer 在12/24/48线程分别达到 15.0 / 28.1 / 39.1 GiB/s",
            fontsize=11, color="#263442")
    ax.text(0.54, 0.16,
            "io_uring q24：48线程达到 71.6K IOPS、25.9 GiB/s",
            fontsize=11, color="#263442")
    ax.text(0.07, 0.105,
            "补充：Parquet/io_uring，16 SSD、48线程，Double Buffer 为 8.0K IOPS、5.79 GiB/s；"
            "相对 Single 分别提高 7.1% 和 6.3%。",
            fontsize=10.2, color=TEAL)
    ax.text(0.055, 0.055,
            "注意：SPDK 是进程内部 completed operations / I/O elapsed；io_uring 与 Parquet 是设备侧 iostat，绝对值不可直接排名。",
            fontsize=9.8, color=GRAY)
    save_slide(fig, output)


def slide_problem_solution(output: Path):
    fig, ax = new_slide("4  问题：双缓冲有效，但不能让所有线程无限扩容", 5)

    ax.text(0.065, 0.81, "现有基线", fontsize=14, weight="bold", color=NAVY)
    baseline = [
        (0.06, "Legacy", "7.70 GiB", "依赖离线大小先验", "#ECEFF1"),
        (0.29, "Static", "31.54 GiB", "查询前完整预分配", "#F1ECFA"),
        (0.52, "Dynamic", "13.16 GiB", "线程独立、重复扩容", "#FDEDE5"),
    ]
    for x, name, rss, note, face in baseline:
        box(ax, (x, 0.59), 0.19, 0.16, f"{name}\n{rss}", face, fontsize=16)
        ax.text(x + 0.095, 0.555, note, fontsize=10, color=GRAY, ha="center")

    arrow(ax, (0.74, 0.67), (0.82, 0.67), color=TEAL)
    box(ax, (0.81, 0.57), 0.14, 0.20, "Selective\n在线容量感知\n负载均衡", "#E8F8F5", fontsize=14)

    ax.plot([0.06, 0.94], [0.48, 0.48], color="#D8E1EA", linewidth=1.2)
    ax.text(0.065, 0.42, "选择性扩容只做三步", fontsize=14, weight="bold", color=NAVY)
    steps = [
        (0.07, "1", "读取 metadata", "获得本文件 column-chunk demand"),
        (0.37, "2", "寻找可复用容量", "路由给 Buffer 足够且队列可接受的 worker"),
        (0.70, "3", "必要时本地扩容", "没有合适接收者时保持本地执行"),
    ]
    for x, number, title, detail in steps:
        ax.text(x, 0.31, number, fontsize=25, weight="bold", color=TEAL,
                ha="center", va="center",
                bbox=dict(boxstyle="circle,pad=.35", facecolor="#E8F8F5", edgecolor=TEAL))
        ax.text(x + 0.045, 0.335, title, fontsize=13, weight="bold", color="#263442")
        ax.text(x + 0.045, 0.275, detail, fontsize=10.5, color=GRAY, wrap=True)
        if number != "3":
            arrow(ax, (x + 0.23, 0.31), (x + 0.27, 0.31), color="#9AA6B2")

    ax.text(0.94, 0.10,
            "策略输入只有 demand、capacity、queue load，可与 reader / I/O 后端解耦",
            fontsize=11.5, color=NAVY, weight="bold", ha="right")
    save_slide(fig, output)


def slide_results(output: Path, figure_path: Path):
    fig, ax = new_slide("5  结果：Selective 在性能与内存之间取得可部署折中", 6)
    image = mpimg.imread(figure_path)
    image_ax = fig.add_axes([0.045, 0.13, 0.61, 0.70])
    image_ax.imshow(image)
    image_ax.axis("off")

    ax.text(0.70, 0.76, "12.25%", fontsize=30, weight="bold", color=TEAL)
    ax.text(0.70, 0.70, "查询时间下降", fontsize=13, color=GRAY)
    ax.text(0.70, 0.58, "24.1%", fontsize=30, weight="bold", color=TEAL)
    ax.text(0.70, 0.52, "RSS 中位数下降", fontsize=13, color=GRAY)
    ax.text(0.70, 0.41, "9.98 GiB", fontsize=25, weight="bold", color=NAVY)
    ax.text(0.70, 0.355, "Selective V1 峰值 RSS 中位数", fontsize=11.5, color=GRAY)

    ax.add_patch(FancyBboxPatch((0.69, 0.15), 0.26, 0.13,
                                boxstyle="round,pad=0.015,rounding_size=0.02",
                                facecolor="#EAF2F8", edgecolor=BLUE, linewidth=1.2))
    ax.text(0.82, 0.215,
            "保留 Double Buffer 并行能力\n避免全局预分配与重复扩容",
            fontsize=12.5, color=NAVY, ha="center", va="center", weight="bold")

    ax.text(0.055, 0.065,
            "边界：Double Buffer 已有跨路径证据；Selective 当前仅完成 Pixels/io_uring 验证，"
            "SPDK 与 Parquet 接入是下一步。",
            fontsize=10.5, color=GRAY)
    save_slide(fig, output)


def content_types(slide_count: int) -> str:
    slides = "".join(
        f'<Override PartName="/ppt/slides/slide{i}.xml" '
        'ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
        for i in range(1, slide_count + 1)
    )
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Default Extension="png" ContentType="image/png"/>
<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>
<Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>
<Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>
<Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
{slides}</Types>'''


def slide_xml() -> str:
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
 xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
 <p:cSld><p:spTree>
  <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
  <p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{EMU_W}" cy="{EMU_H}"/>
   <a:chOff x="0" y="0"/><a:chExt cx="{EMU_W}" cy="{EMU_H}"/></a:xfrm></p:grpSpPr>
  <p:pic><p:nvPicPr><p:cNvPr id="2" name="Slide image"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>
   <p:blipFill><a:blip r:embed="rId2"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>
   <p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{EMU_W}" cy="{EMU_H}"/></a:xfrm>
    <a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:pic>
 </p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sld>'''


def build_pptx(slides: list[Path], output: Path):
    output.parent.mkdir(parents=True, exist_ok=True)
    slide_ids = "".join(
        f'<p:sldId id="{255 + i}" r:id="rId{i + 1}"/>'
        for i in range(1, len(slides) + 1)
    )
    presentation = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:presentation xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
 xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
 <p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>
 <p:sldIdLst>{slide_ids}</p:sldIdLst>
 <p:sldSz cx="{EMU_W}" cy="{EMU_H}" type="screen16x9"/><p:notesSz cx="6858000" cy="9144000"/>
</p:presentation>'''
    rels = ['<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>']
    rels.extend(
        f'<Relationship Id="rId{i + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide{i}.xml"/>'
        for i in range(1, len(slides) + 1)
    )
    presentation_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                         '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                         + "".join(rels) + '</Relationships>')

    master = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldMaster xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld>
<p:clrMap accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" bg1="lt1" bg2="lt2" folHlink="folHlink" hlink="hlink" tx1="dk1" tx2="dk2"/>
<p:sldLayoutIdLst><p:sldLayoutId id="1" r:id="rId1"/></p:sldLayoutIdLst><p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>'''
    layout = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldLayout xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" type="blank" preserve="1">
<p:cSld name="Blank"><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>'''
    theme = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Buffer Research"><a:themeElements>
<a:clrScheme name="Default"><a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="17365D"/></a:dk2><a:lt2><a:srgbClr val="F3F6F9"/></a:lt2><a:accent1><a:srgbClr val="2878B5"/></a:accent1><a:accent2><a:srgbClr val="2A9D8F"/></a:accent2><a:accent3><a:srgbClr val="D95F02"/></a:accent3><a:accent4><a:srgbClr val="6C5CE7"/></a:accent4><a:accent5><a:srgbClr val="7F8C8D"/></a:accent5><a:accent6><a:srgbClr val="E0B44C"/></a:accent6><a:hlink><a:srgbClr val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>
<a:fontScheme name="Default"><a:majorFont><a:latin typeface="Arial"/><a:ea typeface="Noto Sans CJK SC"/><a:cs typeface="Arial"/></a:majorFont><a:minorFont><a:latin typeface="Arial"/><a:ea typeface="Noto Sans CJK SC"/><a:cs typeface="Arial"/></a:minorFont></a:fontScheme>
<a:fmtScheme name="Default"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w="9525"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme>
</a:themeElements></a:theme>'''

    root_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/></Relationships>'''
    master_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="../theme/theme1.xml"/></Relationships>'''
    layout_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="../slideMasters/slideMaster1.xml"/></Relationships>'''
    core = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>Double Buffer 与选择性 Buffer 扩容</dc:title><dc:creator>Pixels Research</dc:creator><dcterms:created xsi:type="dcterms:W3CDTF">2026-09-14T00:00:00Z</dcterms:created></cp:coreProperties>'''
    app = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>Microsoft Office PowerPoint</Application><PresentationFormat>On-screen Show (16:9)</PresentationFormat><Slides>{len(slides)}</Slides></Properties>'''

    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as package:
        package.writestr("[Content_Types].xml", content_types(len(slides)))
        package.writestr("_rels/.rels", root_rels)
        package.writestr("docProps/core.xml", core)
        package.writestr("docProps/app.xml", app)
        package.writestr("ppt/presentation.xml", presentation)
        package.writestr("ppt/_rels/presentation.xml.rels", presentation_rels)
        package.writestr("ppt/slideMasters/slideMaster1.xml", master)
        package.writestr("ppt/slideMasters/_rels/slideMaster1.xml.rels", master_rels)
        package.writestr("ppt/slideLayouts/slideLayout1.xml", layout)
        package.writestr("ppt/slideLayouts/_rels/slideLayout1.xml.rels", layout_rels)
        package.writestr("ppt/theme/theme1.xml", theme)
        for index, slide in enumerate(slides, 1):
            package.writestr(f"ppt/slides/slide{index}.xml", slide_xml())
            slide_rels = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/slide{index}.png"/></Relationships>'''
            package.writestr(f"ppt/slides/_rels/slide{index}.xml.rels", slide_rels)
            package.write(slide, f"ppt/media/slide{index}.png")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--figures", type=Path,
        default=Path("docs/buffer-pool/figures/buffer-research-overview"),
    )
    parser.add_argument(
        "--slide-images", type=Path,
        default=Path("docs/buffer-pool/figures/buffer-research-slides"),
    )
    parser.add_argument(
        "--output", type=Path,
        default=Path("docs/buffer-pool/buffer-research-overview.pptx"),
    )
    args = parser.parse_args()
    configure_font()
    args.slide_images.mkdir(parents=True, exist_ok=True)
    slides = [args.slide_images / f"slide-{index}.png" for index in range(1, 7)]
    slide_title(slides[0])
    slide_backend(slides[1], args.figures / "01-doublebuffer-cross-backend.png")
    slide_raw_io(slides[2])
    slide_doublebuffer_io(slides[3])
    slide_problem_solution(slides[4])
    slide_results(slides[5], args.figures / "03-buffer-pool-tradeoff.png")
    build_pptx(slides, args.output)


if __name__ == "__main__":
    main()
