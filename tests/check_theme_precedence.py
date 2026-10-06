"""check_theme_precedence.py - 保证显式界面主题优先于文档纸张方案。

sync_theme 的规则是：跟随系统时，深色纸张可以令 chrome 变暗；显式浅色/深色时，
纸张方案不能覆盖界面明暗选择。这个行为依赖平台代码，先用结构断言钉住分支形状。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLATFORM = REPO / "src" / "app" / "platform.cpp"
UI = REPO / "src" / "app" / "ui.cpp"


def main() -> int:
    src = PLATFORM.read_text(encoding="utf-8")
    try:
        body = src.split("void sync_theme(int scheme)", 1)[1].split("// ---------------- D3D11", 1)[0]
    except IndexError:
        print("找不到 sync_theme() 或其边界", file=sys.stderr)
        return 1

    failed = False
    if re.search(r"const bool follows_system\s*=\s*g_prefs\.theme\s*==\s*0", body):
        print("  [PASS] 深色纸张联动只在跟随系统时启用")
    else:
        print("  [FAIL] sync_theme 缺少 follows_system 守门变量")
        failed = True

    if re.search(r"follows_system\s*&&\s*\(", body) and "scheme == 1" in body:
        print("  [PASS] 深色纸张只作为跟随系统的额外变暗条件")
    else:
        print("  [FAIL] 深色纸张联动没有嵌在 follows_system 条件内")
        failed = True

    if re.search(r"system_prefers_dark_cached\(\)\s*\)\s*\|\|\s*\(\s*scheme\s*==\s*1", body):
        print("  [FAIL] 发现会覆盖显式界面主题的无条件 scheme == 1")
        failed = True
    else:
        print("  [PASS] 未发现无条件覆盖显式界面主题的 scheme == 1")

    ui = UI.read_text(encoding="utf-8")
    if 'with_icon(kIcPalette, "外观")' in ui:
        print("  [PASS] 合并后的菜单入口命名为“外观”")
    else:
        print("  [FAIL] 未找到“外观”菜单入口")
        failed = True

    if 'kThemeNames[] = { "跟随系统", "浅色", "深色" }' in ui and \
       'kPageSchemeNames[] = { "原色", "深色纸张", "暖色" }' in ui:
        print("  [PASS] 界面主题与纸张名称各有一份共用标签表")
    else:
        print("  [FAIL] 主题名称缺失或未集中到共用标签表")
        failed = True

    try:
        appearance = ui.split("void draw_appearance_menu_contents(bool reading)", 1)[1].split(
            "void draw_view_menu_contents()", 1)[0]
        settings = ui.split("void draw_settings_interface_tab()", 1)[1].split(
            "void draw_settings_reading_tab()", 1)[0]
    except IndexError:
        print("  [FAIL] 找不到外观菜单或界面设置实现", file=sys.stderr)
        failed = True
    else:
        if 'SeparatorText("界面（全局）")' in appearance and \
           'SeparatorText("纸张（本书）")' in appearance:
            print("  [PASS] 外观菜单明确标识全局与随文档范围")
        else:
            print("  [FAIL] 外观菜单分组未表达设置范围")
            failed = True

        row_order = [settings.find(f'settings_row("{label}")') for label in
                     ("界面主题", "纸张方案", "联动规则")]
        if all(pos >= 0 for pos in row_order) and row_order == sorted(row_order):
            print("  [PASS] 设置页相邻展示界面主题、纸张方案与联动规则")
        else:
            print("  [FAIL] 设置页缺少相邻的主题选项与联动规则说明")
            failed = True

    print("全部通过。" if not failed else "存在失败用例。")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
