"""check_theme_reset.py — 断言主题切换会先**整套重置** ImGui 样式（自动化测试项）

判据（任一不满足 → 退出码非 0）：
  1. `apply_theme_colors()` 的深色分支必须以 `ImGui::StyleColorsDark(&st)` 打头；
  2. 浅色分支必须包含 `ImGui::StyleColorsLight(&st)`。

为什么单独立一条断言：ImGui 有 60 多个 `ImGuiCol_*`，而我们的分支只覆盖其中三十来个。
"只覆盖在意的那些"看起来更省事，实际会留下**跨主题残留** —— 未覆盖的项沿用上一次留在
style 里的值，于是"先浅色后深色"时它们带着浅色值活到深色主题里。实测踩到的就是
`ImGuiCol_CheckboxSelectedBg`（勾选框选中态的底，浅色是近白 (0.95,0.97,1.00)）：
深色下勾选框变成一整块亮奶油色，和周围完全割裂，肉眼一看就不对，但代码里毫无痕迹。

先整套重置（`StyleColorsLight` / `StyleColorsDark`）再逐项覆盖，这类残留就在结构上不可能
发生 —— 所以这条断言比逐个钉死颜色项更稳、也更不容易腐化。

用法： python tests/check_theme_reset.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLATFORM = REPO / "src" / "app" / "platform.cpp"


def main() -> int:
    src = PLATFORM.read_text(encoding="utf-8")
    if "void apply_theme_colors()" not in src:
        print(f"找不到 apply_theme_colors()：{PLATFORM}", file=sys.stderr)
        return 1

    body = src.split("void apply_theme_colors()", 1)[1]
    try:
        dark = body.split("if (g_app.dark_theme) {", 1)[1].split("} else {", 1)[0]
        light = body.split("} else {", 1)[1].split("if (g_app.tone.active)", 1)[0]
    except IndexError:
        print("apply_theme_colors() 结构与预期不符（找不到深色/浅色分支）", file=sys.stderr)
        return 1

    failed = False

    if re.search(r"ImGui::StyleColorsDark\s*\(\s*&st\s*\)", dark):
        print("  [PASS] 深色分支先整套重置（StyleColorsDark）")
    else:
        failed = True
        print("  [FAIL] 深色分支缺少 StyleColorsDark —— 未覆盖的颜色项会残留上一个主题的值")
        print("         （曾导致：深色下勾选框整块变成亮奶油色）")

    if re.search(r"ImGui::StyleColorsLight\s*\(\s*&st\s*\)", light):
        print("  [PASS] 浅色分支整套重置（StyleColorsLight）")
    else:
        failed = True
        print("  [FAIL] 浅色分支缺少 StyleColorsLight")

    # 深色分支里"我们主动改过"的项越多越好，但至少要覆盖勾选框选中态 ——
    # 它的默认值是从 FrameBg 派生的强调色，和我们"深底 + 彩色勾"的一贯做法不同。
    if "ImGuiCol_CheckboxSelectedBg" in dark:
        print("  [PASS] 深色分支显式设置勾选框选中态底色")
    else:
        failed = True
        print("  [FAIL] 深色分支未设置 ImGuiCol_CheckboxSelectedBg")

    print("全部通过。" if not failed else "存在失败用例。")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
