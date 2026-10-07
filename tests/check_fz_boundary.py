#!/usr/bin/env python3
"""document 层 fz_* 边界检查（ADR-083 遗留，路线图第五批）。

背景：`document.cpp` 头顶铁律第 1 条要求"任何 `fz_*` 调用都必须被 `fz_try/fz_catch` 包住"，
但整篇 `document.cpp` **并不**满足这条字面规则 —— 设备回调转发（`SplitDevice`）、单份
pixmap 的读取（`apply_scheme_lut`）、`fz_caught_message` 的包装函数等都在 `fz_try` 之外
调用 `fz_*`（它们不抛错）。因此"扫描所有 fz_* 是否在 fz_try 内"会得到大量并非缺陷的命中。

ADR-083 真正建立、可结构断言的是**销毁路径与属性访问器**上的边界纪律：

  1. 四个边界函数存在且各自包了 `fz_try`：`pixmap_attrs` / `drop_pixmap_safe` /
     `drop_stext_safe` / `drop_document_safe`；
  2. `PageBitmap::reset` 的销毁走 `drop_pixmap_safe`（不再裸调 `fz_drop_pixmap`）；
  3. 会话/文档的两条拆解路径确实用了 `drop_stext_safe` / `drop_document_safe`；
  4. `PageBitmap` 的 `width/height/stride/samples` 是**纯访问器**（零 `fz_*` 回调，
     属性在构造时快照，ADR-083 决策 2）。

本脚本按函数体（大括号配对，先剥离注释与字符串字面量）逐条断言这些事实 —— 它是"边界
函数 + 代码评审"这条软规则的可执行版本。退出码：任一不成立即非零。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src" / "document" / "document.cpp"


def strip_noise(src: str) -> str:
    """把注释与字符串/字符字面量替换为等长空白（保留换行，行号不变）。"""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in ('"', "'"):
            q = c
            j = i + 1
            while j < n and src[j] != q:
                if src[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def body_of(s: str, header: str) -> str | None:
    """定位函数头 `header`（精确子串），返回其后第一个 `{...}` 的正文；找不到返回 None。"""
    m = re.search(header, s)
    if not m:
        return None
    b = s.find("{", m.end())
    if b < 0:
        return None
    depth = 0
    j = b
    while j < len(s):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    return s[b : j + 1]


def main() -> int:
    if not SRC.is_file():
        print(f"找不到 {SRC}", file=sys.stderr)
        return 1
    s = strip_noise(SRC.read_text(encoding="utf-8"))
    failed = 0

    print(f"check_fz_boundary: {SRC.name} —— ADR-083 边界纪律")

    # 1) 四个边界函数存在且包了 fz_try
    helpers = ["PixmapAttrs pixmap_attrs(", "void drop_pixmap_safe(",
               "void drop_stext_safe(", "void drop_document_safe("]
    for h in helpers:
        body = body_of(s, re.escape(h))
        name = h.split()[-1].rstrip("(")
        if body is not None and "fz_try(" in body:
            print(f"  [PASS] {name}：定义存在且包了 fz_try")
        else:
            print(f"  [FAIL] {name}：未找到定义，或其体内没有 fz_try")
            failed += 1

    # 2) PageBitmap::reset 销毁走 drop_pixmap_safe（不裸调 fz_drop_pixmap）
    reset = body_of(s, r"void\s+PageBitmap::reset\s*\(")
    if reset is None:
        print("  [FAIL] 找不到 PageBitmap::reset")
        failed += 1
    elif "drop_pixmap_safe(" in reset and "fz_drop_pixmap(" not in reset:
        print("  [PASS] PageBitmap::reset：销毁走 drop_pixmap_safe（无裸 fz_drop_pixmap）")
    else:
        print("  [FAIL] PageBitmap::reset：应走 drop_pixmap_safe，且不得裸调 fz_drop_pixmap")
        failed += 1

    # 3) 拆解路径确实用了 *_safe（弱断言：调用点存在）
    for call, label in [("drop_stext_safe(", "stext 拆解"), ("drop_document_safe(", "document 拆解")]:
        if call in s:
            print(f"  [PASS] {label}：经 {call.rstrip('(')}")
        else:
            print(f"  [FAIL] {label}：未见 {call} 调用点")
            failed += 1

    # 4) PageBitmap 属性访问器为纯访问器（零 fz_*）
    for acc in ["width", "height", "stride", "samples"]:
        body = body_of(s, r"PageBitmap::" + acc + r"\s*\(")
        if body is None:
            print(f"  [FAIL] 找不到 PageBitmap::{acc}")
            failed += 1
        elif re.search(r"\bfz_\w+\s*\(", body):
            print(f"  [FAIL] PageBitmap::{acc}：访问器内仍有 fz_* 回调（应为快照访问器）")
            failed += 1
        else:
            print(f"  [PASS] PageBitmap::{acc}：纯访问器（零 fz_*）")

    if failed:
        print("  存在不满足的边界纪律（ADR-083）。")
        return 1
    print("  OK：ADR-083 边界纪律全部成立")
    return 0


if __name__ == "__main__":
    sys.exit(main())
