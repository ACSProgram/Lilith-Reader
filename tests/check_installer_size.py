#!/usr/bin/env python3
"""check_installer_size.py — 安装包体积门禁（ADR-084）

断言 installer/dist/ 下的安装包不超过体积上限，超限非零退出。
上限依据 docs/05-体积预算.md §5：0.6.0 打包实测 24.9 MiB（对应裁剪前 40 MiB exe），
字体裁剪后 exe 38.06 MiB、预计安装包约 23 MiB；35 MiB 的余量专防"字体意外回到
全量"一类大回归（lzma2 后约 40 MiB 会触发），日常微膨胀不报警。

用法：python tests/check_installer_size.py <installer/dist 目录>
仅用标准库（CI 的 installer job 不装任何依赖）；本地亦可直接运行。

输出一律 UTF-8（sys.stdout.reconfigure）：子进程 stdout 为管道时 Python 默认用
系统 ANSI 代码页，在 cp1252 的 runner 上打印中文会抛 UnicodeEncodeError ——
ADR-076 勘误二的教训，这里在脚本层自洽，不依赖调用方设置 PYTHONUTF8。
"""

import sys
from pathlib import Path

# 体积上限：35 MiB（依据见文件头注释与 docs/05 §5）
BUDGET_BYTES = 35 * 1024 * 1024


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if len(sys.argv) != 2:
        print("用法: python tests/check_installer_size.py <installer/dist 目录>")
        return 2

    dist = Path(sys.argv[1])
    installers = sorted(dist.glob("*.exe")) if dist.is_dir() else []
    if not installers:
        print(f"check_installer_size: 目录里没有安装包: {dist}")
        return 1
    if len(installers) > 1:
        print("check_installer_size: 期望恰好一个安装包，实际有 " + str(len(installers)))
        for p in installers:
            print("  " + p.name)
        return 1

    exe = installers[0]
    size = exe.stat().st_size
    mib = size / (1024 * 1024)
    print(f"check_installer_size: {exe.name} = {size:,} bytes ({mib:.2f} MiB)")
    print(f"  预算上限 {BUDGET_BYTES:,} bytes (35 MiB)，出处 docs/05-体积预算.md §5")

    if size > BUDGET_BYTES:
        over = size - BUDGET_BYTES
        print(f"  超预算 {over:,} bytes —— 先看 docs/05 §4 的裁剪手段，或经 ADR 调整上限")
        return 1

    print("  通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
