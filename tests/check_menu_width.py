#!/usr/bin/env python3
"""菜单文案宽度断言（ADR-067 的配套防线）。

背景：弹出菜单的宽度由**最长的那一项**决定。只要有人往菜单里塞一条特别长的文案
（"双页对开（书籍模式）"那种），整张菜单就被撑宽、其余短项显得空荡 —— 这是人工反馈过的
感官问题，也很容易在后续加功能时复发。补充说明一律走悬停提示（menu_item 的 tip 参数），
不进文案。

判据：src/app/ui.cpp 里所有"菜单项文案"的**显示宽度**（CJK/全角算 2 列，其余算 1 列）
不得超过 MAX_COLS。

被检查的写法（新增菜单项时请沿用，否则可能漏检 —— 新写法出现时同步扩充 PATTERNS）：
    menu_item(kIcX, "文案", ...)
    menu_item_cmd(kIcX, "文案", ...)
    menu_item_cmd(kIcX, cond ? "文案甲" : "文案乙", ...)
    with_icon(kIcX, "文案")
    ImGui::MenuItem("文案", ...)

退出码：任一超宽即非零。
"""

import os
import re
import sys
import unicodedata

MAX_COLS = 12

PATTERNS = [
    re.compile(r'menu_item(?:_cmd)?\(\s*kIc\w+\s*,\s*"([^"]*)"'),
    re.compile(r'menu_item(?:_cmd)?\(\s*kIc\w+\s*,[^?"]*\?\s*"([^"]*)"\s*:\s*"([^"]*)"'),
    re.compile(r'with_icon\(\s*kIc\w+\s*,\s*"([^"]*)"'),
    re.compile(r'ImGui::MenuItem\(\s*"([^"]*)"'),
]


def disp_width(s):
    """显示宽度：东亚宽/全角字符按 2 列，其余按 1 列。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in s)


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(repo, "src", "app", "ui.cpp")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    found = []          # (line, label)
    for pat in PATTERNS:
        for m in pat.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            for g in m.groups():
                if g:
                    found.append((line, g))

    if not found:
        print("check_menu_width: 未提取到任何菜单文案 —— 写法可能变了，请同步 PATTERNS")
        return 1

    bad = [(ln, lab, disp_width(lab)) for ln, lab in found if disp_width(lab) > MAX_COLS]
    print(f"check_menu_width: 检查 {len(found)} 条菜单文案，上限 {MAX_COLS} 列")
    for ln, lab in sorted(found, key=lambda t: -disp_width(t[1]))[:5]:
        print(f"  最长：{disp_width(lab):2d} 列  ui.cpp:{ln}  {lab}")
    if bad:
        print("")
        for ln, lab, w in sorted(bad):
            print(f"  [FAIL] ui.cpp:{ln}  {w} 列（上限 {MAX_COLS}）：{lab}")
        print("  说明文字请改用 menu_item 的 tip 参数（悬停提示），不要写进文案。")
        return 1

    print("  OK：全部在宽度上限内")
    return 0


if __name__ == "__main__":
    sys.exit(main())
