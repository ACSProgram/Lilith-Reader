# check_icons.py — 图标资源断言（接入 tests/run_tests.ps1）
#
# 为什么需要：ICO 是"目录 + 每项一段位图"的容器，最容易出的错不是画得不好，而是
# **帧集不全或帧尺寸名不副实**——Windows 会拿现有帧去拉伸，表现为"图标发糊 + 一圈灰边"。
# 曾经的真实事故：生成脚本把 16px 帧当基准图交给 Pillow 保存，Pillow 会跳过所有
# "大于基准图"的槽位，最终 .ico 里只有 16×16 一帧；Explorer 把它放大到 48/256，
# 于是整套文件图标全是糊的。
#
# 断言内容（任一不满足即非零退出）：
#   1. 每个 .ico 的帧集与预期完全一致（尺寸、数量）
#   2. 每个目录项声明的尺寸 == 该项位图解码出来的真实尺寸（防"小图塞进大槽位"）
#   3. 每帧都有实际像素内容（非全透明）
#
# 用法：python assets/check_icons.py
# 依赖：Pillow

from PIL import Image
import io
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORMAT_DIR = os.path.join(REPO, "installer", "icons")
APP_ICON = os.path.join(REPO, "assets", "icon.ico")

# 与 assets/make_format_icons.py 的 ICO_SIZES 保持一致
FORMAT_SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 160, 192, 256]
# 与 assets/make_app_icon.py 的 APP_ICON_SIZES 保持一致
APP_SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 160, 192, 256]
FORMATS = ["PDF", "EPUB", "MOBI", "FB2", "CBZ", "XPS", "IMG"]

failures = []
checks = 0


def read_dir(path):
    """解析 ICO 目录，返回 [(w, h, offset, size), ...]。"""
    with open(path, "rb") as f:
        data = f.read()
    reserved, typ, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or typ != 1:
        raise ValueError("不是合法的 ICO 文件")
    entries = []
    off = 6
    for _ in range(count):
        w, h, _cc, _res, _pl, _bpp, size, offset = struct.unpack("<BBBBHHII", data[off:off + 16])
        entries.append((w or 256, h or 256, offset, size))
        off += 16
    return data, entries


def check_ico(path, expected, label):
    global checks
    name = os.path.relpath(path, REPO)
    if not os.path.isfile(path):
        failures.append(f"{name}: 文件不存在")
        return
    try:
        data, entries = read_dir(path)
    except Exception as e:                                  # noqa: BLE001
        failures.append(f"{name}: 解析失败 {e}")
        return

    got = sorted(w for w, _h, _o, _s in entries)
    checks += 1
    if got != sorted(expected):
        failures.append(f"{name}: 帧集不符\n    期望 {sorted(expected)}\n    实际 {got}")
        return

    for w, h, offset, size in entries:
        checks += 1
        if w != h:
            failures.append(f"{name}: 帧 {w}×{h} 不是正方形")
            continue
        payload = data[offset:offset + size]
        try:
            im = Image.open(io.BytesIO(payload)).convert("RGBA")
        except Exception as e:                              # noqa: BLE001
            failures.append(f"{name}: {w}px 帧解码失败 {e}")
            continue
        if im.size != (w, h):
            failures.append(f"{name}: {w}px 槽位里装的其实是 {im.size[0]}×{im.size[1]} 位图")
        checks += 1
        if im.getchannel("A").getbbox() is None:
            failures.append(f"{name}: {w}px 帧全透明（没有实际内容）")

        # 四角必须透明：纸面以外的画布区域不能有底色，否则 Explorer 选中（蓝色高亮）时
        # 会露出一个白方块。曾因"纸面填充铺满整块画布、没裁到轮廓"踩过这个坑。
        for cx, cy in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
            checks += 1
            a = im.getpixel((cx, cy))[3]
            if a > 16:
                failures.append(f"{name}: {w}px 帧四角不透明（({cx},{cy}) alpha={a}）"
                                f"—— 选中时会露出白底")
                break

    print(f"  ok  {name}  ({len(entries)} 帧，{label})")


def main():
    print("图标资源断言：")
    for fmt in FORMATS:
        check_ico(os.path.join(FORMAT_DIR, f"lilith_{fmt}.ico"), FORMAT_SIZES, "文件类型图标")
    check_ico(APP_ICON, APP_SIZES, "应用图标")

    print(f"\n共 {checks} 项断言。")
    if failures:
        print(f"存在 {len(failures)} 项失败：")
        for f in failures:
            print("  FAIL " + f)
        return 1
    print("图标资源全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
