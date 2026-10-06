"""check_font_coverage.py — 断言内嵌字体子集覆盖界面所需的全部字符（自动化测试项）

判据（任一不满足 → 退出码非 0）：
  1. **源码字符集**：`src/`（去注释后）里出现的每一个非 ASCII 字符都在子集 cmap 中。
     这条保证"界面自述文字不依赖系统字体"——缺字就会在界面上显示成豆腐块。
  2. **GB2312 一级字表（3755 常用字）**：全部在子集中。这条覆盖文档侧文字
     （文件名、PDF 目录标题）的常用部分；更生僻的字由运行时 fallback（msyh.ttc）兜底。

为什么做成脚本而不是 C++ 用例：断言对象是**字体文件的 cmap**，用 fontTools 读表最直接；
在 C++ 里复刻一份 cmap 解析只会增加出错面，证据强度并不更高。
字符集的构造逻辑与生成脚本共用（`make_font_subset`），避免两处漂移。
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_font_subset as mfs  # noqa: E402  （同目录脚本，复用字符集构造与去注释扫描）


def main() -> int:
    subset = mfs.REPO / "assets" / mfs.OUT_NAME
    if not subset.is_file():
        print(f"找不到内嵌字体子集：{subset}\n请先运行 assets/make_font_subset.py", file=sys.stderr)
        return 1

    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("需要 fontTools：pip install fonttools", file=sys.stderr)
        return 1

    font = TTFont(str(subset), lazy=True)
    cmap: set[int] = set()
    for table in font["cmap"].tables:
        cmap |= set(table.cmap.keys())
    font.close()

    src_chars, where = mfs.source_chars()
    gb = mfs.gb2312_level1()
    print(f"  子集：{subset.relative_to(mfs.REPO)}  {subset.stat().st_size:,} 字节")
    print(f"  子集 cmap 码位数：{len(cmap)}")

    failed = False
    missing_src = sorted(c for c in src_chars if ord(c) not in cmap)
    print(f"  源码非 ASCII 字符：{len(src_chars)}  缺失 {len(missing_src)}")
    if missing_src:
        failed = True
        for c in missing_src[:20]:
            print(f"    ** 缺字 U+{ord(c):04X} {c!r} <- {', '.join(where.get(c, [])[:3])}")

    missing_gb = sorted(c for c in gb if ord(c) not in cmap)
    print(f"  GB2312 一级字表：{len(gb)}  缺失 {len(missing_gb)}")
    if missing_gb:
        failed = True
        print(f"    ** 缺字：{' '.join(f'U+{ord(c):04X}' for c in missing_gb[:20])}")

    if failed:
        print("字体子集覆盖不足：请重新生成（assets/make_font_subset.py）。", file=sys.stderr)
        return 1
    print("字体子集覆盖断言通过。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
