"""make_font_subset.py — 生成 UI 字体子集（Phase 6，ADR-048）

为什么需要它
------------
界面文字原先依赖系统字体 `C:\\Windows\\Fonts\\msyh.ttc`（微软雅黑）。把它换成
**随 exe 内嵌的子集**后：界面观感不再随机器/系统版本漂移，也不必依赖用户是否装了某款字体。
代价是子集必须"够用"—— 缺字会直接显示成豆腐块，所以覆盖范围必须是可程序判定的。

字符集构成（四项并集）
----------------------
1. **源码字符集**：`src/` 下全部 `.cpp/.h/.ixx/.rc` 与 `tests/` 下 `.cpp` 里出现的
   每一个非 ASCII 字符。界面自述文字（按钮/菜单/提示/帮助）由此 100% 覆盖 ——
   这条是硬约束，由 `assets/check_font_coverage.py` 在 `tests/run_tests.ps1` 里断言。
2. **ASCII 可见区**：0x20~0x7E。
3. **GB2312 一级字表（3755 个常用汉字）**：覆盖文档侧文字（文件名、PDF 目录标题）。
   由 Python 的 `gb2312` 编解码器**离线枚举**（区 16~55），不依赖任何外部字表文件。
4. **常用标点/符号区段**：拉丁补充、通用标点、箭头、数学、圈号、几何、CJK 标点、全角形式。

超出子集的生僻字由运行时 fallback（系统 `msyh.ttc`）补，缺失也不影响界面自身文字的完整性。

源字体
------
Noto Sans SC Regular（OFL 1.1，思源黑体同源；可自由嵌入分发），约 8.3MB。
下载：见 SOURCE_URL。源字体**不入库**（体积大且可再取），只提交生成物。

用法
----
    python assets/make_font_subset.py                 # 生成 assets/ui_font_subset.otf
    python assets/make_font_subset.py --source <路径>  # 指定源字体
环境变量 LILITH_FONT_SOURCE 可覆盖默认源字体路径（默认取本机缓存目录）。
"""

from __future__ import annotations

import argparse
import os
import sys
import unicodedata
from pathlib import Path

SOURCE_URL = ("https://cdn.jsdelivr.net/gh/notofonts/noto-cjk@main/"
              "Sans/SubsetOTF/SC/NotoSansSC-Regular.otf")
# 源字体路径不写死机器路径：优先环境变量 LILITH_FONT_SOURCE，否则落在本机缓存目录
# %LOCALAPPDATA%\LilithReader\fonts-src\（源字体不入库，只提交生成物）。
DEFAULT_SOURCE = Path(os.environ.get("LILITH_FONT_SOURCE") or
                      Path(os.environ.get("LOCALAPPDATA") or Path.home()) /
                      "LilithReader" / "fonts-src" / "NotoSansSC-Regular.otf")
OUT_NAME = "ui_font_subset.otf"

REPO = Path(__file__).resolve().parent.parent

# 常用标点/符号区段（含界面里实际用到的 × ° · − … ★ ← → 等）
EXTRA_RANGES = [
    (0x00A0, 0x00FF),   # 拉丁补充：×  °  ·  ÷
    (0x2000, 0x206F),   # 通用标点：– — ‘ ’ “ ” … ‰
    (0x2070, 0x209F),   # 上下标
    (0x20A0, 0x20BF),   # 货币符号：€
    (0x2100, 0x214F),   # 字母式符号：℃ №
    (0x2190, 0x21FF),   # 箭头：← → ↑ ↓
    (0x2200, 0x22FF),   # 数学运算符：− ≈ ≤ ≥ ≠
    (0x2460, 0x24FF),   # 带圈字符：① ⑵
    (0x25A0, 0x25FF),   # 几何图形：■ □ ▲ ▼
    (0x2600, 0x26FF),   # 杂项符号：★ ☆ ☀
    (0x3000, 0x303F),   # CJK 标点：《》「」、。·
    (0xFF00, 0xFFEF),   # 全角形式：Ａ（）：．％
]

SRC_SUFFIXES = {".cpp", ".h", ".hpp", ".ixx", ".rc", ".py", ".ps1"}


def gb2312_level1() -> set[str]:
    """GB2312 一级字表（区 16~55，3755 字）：按码位枚举并解码，不依赖外部字表。"""
    out: set[str] = set()
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes((hi, lo)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ch)
    return out


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释，只保留可能被渲染的代码文本。

    为什么要去注释：注释里的"☰"这类字符并不参与渲染，若把它们计入硬约束，
    就会因为源字体没有该字形而误报缺字（实测第一版即因此失败）。
    用最小状态机而不是正则：字符串字面量里的 `//`（URL）不能被当成注释起点。
    """
    out: list[str] = []
    i, n = 0, len(text)
    state = "code"          # code | line | block | str | chr
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; i += 2; continue
            if c == '"':
                state = "str"; out.append(c); i += 1; continue
            if c == "'":
                state = "chr"; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line":
            if c == "\n":
                state = "code"; out.append(c)
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "code"; i += 2; continue
            if c == "\n":
                out.append(c)   # 保留换行，行号/位置仍然可读
            i += 1; continue
        # str / chr
        out.append(c)
        if c == "\\":
            if i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
        elif (state == "str" and c == '"') or (state == "chr" and c == "'"):
            state = "code"
        i += 1
    return "".join(out)


def source_chars() -> tuple[set[str], dict[str, list[str]]]:
    """扫描源码（去注释后）里出现的非 ASCII 字符，同时记录出处便于排查。"""
    chars: set[str] = set()
    where: dict[str, list[str]] = {}
    roots = [REPO / "src", REPO / "tests", REPO / "assets"]
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SRC_SUFFIXES:
                continue
            if path.resolve() == Path(__file__).resolve():
                continue
            # 跳过构建产物目录：里面的文件是生成物（测试日志、临时脚本、中间产物），
            # 不属于"界面自述文字"。把它们算进字符集只会制造假失败
            # （例如日志里的原文输出把 U+2315 这类符号带进来）。
            if "_build" in path.parts:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for ch in strip_comments(text):
                if ord(ch) > 0x7F and usable(ch):
                    chars.add(ch)
                    where.setdefault(ch, []).append(str(path.relative_to(REPO)))
    return chars, where


def usable(ch: str) -> bool:
    """排除控制/格式/未分配码位（ZWJ、行分隔符等）—— 它们既不该进字体也不该进断言。"""
    cat = unicodedata.category(ch)
    return cat[0] != "C" and cat not in ("Zl", "Zp")


def build_charset(source_cmap: set[int]) -> tuple[set[str], set[str], dict[str, list[str]]]:
    """返回 (硬约束字符集, 软字符集, 源码字符出处)。

    硬约束 = ASCII + GB2312 一级 + 源码非 ASCII：缺一个就是缺陷，必须由源字体覆盖；
    软字符 = 常用标点/符号区段里**源字体确实有**的那些（有则锦上添花，没有不算问题）。
    """
    ascii_chars = {chr(c) for c in range(0x20, 0x7F)}
    gb = gb2312_level1()
    src, where = source_chars()

    hard = {c for c in (ascii_chars | gb | src) if usable(c)}
    soft = {chr(c) for lo, hi in EXTRA_RANGES for c in range(lo, hi + 1)
            if usable(chr(c)) and c in source_cmap} - hard

    print(f"  ASCII 可见区      : {len(ascii_chars)}")
    print(f"  GB2312 一级字表   : {len(gb)}")
    print(f"  源码非 ASCII 字符 : {len(src)}"
          f"（来自 {len({f for v in where.values() for f in v})} 个文件）")
    print(f"  → 硬约束合计      : {len(hard)}")
    print(f"  常用符号（软）    : {len(soft)}")
    return hard, soft, where


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--out", type=Path, default=REPO / "assets" / OUT_NAME)
    args = ap.parse_args()

    if not args.source.is_file():
        print(f"找不到源字体：{args.source}\n请先下载：{SOURCE_URL}", file=sys.stderr)
        return 1

    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print("需要 fontTools：pip install fonttools", file=sys.stderr)
        return 1

    src_font = TTFont(str(args.source), lazy=True)
    src_cmap: set[int] = set()
    for table in src_font["cmap"].tables:
        src_cmap |= set(table.cmap.keys())
    src_font.close()

    print("== 字符集 ==")
    hard, soft, where = build_charset(src_cmap)
    missing_hard = sorted(c for c in hard if ord(c) not in src_cmap)
    if missing_hard:
        print(f"\n**源字体缺少硬约束字符 {len(missing_hard)} 个**（界面会出现豆腐块）：")
        for c in missing_hard[:40]:
            print(f"    U+{ord(c):04X} {c!r}  <- {', '.join(where.get(c, ['(ASCII/常用字表)'])[:3])}")
        print("  需换源字体或把这些字加入 fallback 覆盖。")
        return 2

    charset = hard | soft

    print("\n== 子集化 ==")
    opts = subset.Options()
    opts.drop_tables += ["DSIG", "VORG"]
    opts.layout_features = ["*"]      # 保留 kern 等排版特性
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.glyph_names = False
    font = subset.load_font(str(args.source), opts)
    sub = subset.Subsetter(options=opts)
    sub.populate(unicodes={ord(c) for c in charset})
    sub.subset(font)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(args.out), opts)
    font.close()

    # 覆盖自检：子集必须覆盖硬约束的每一个字符
    check = TTFont(str(args.out), lazy=True)
    cmap: set[int] = set()
    for table in check["cmap"].tables:
        cmap |= set(table.cmap.keys())
    check.close()
    missing = sorted(c for c in hard if ord(c) not in cmap)
    size = args.out.stat().st_size
    print(f"  生成：{args.out.relative_to(REPO)}  {size:,} 字节 ({size/1048576:.2f} MiB)")
    print(f"  硬约束覆盖：{len(hard) - len(missing)}/{len(hard)}")
    if missing:
        print(f"  **子集缺字 {len(missing)} 个**：{' '.join(f'U+{ord(c):04X}' for c in missing[:20])}")
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
