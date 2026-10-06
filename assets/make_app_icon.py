# make_app_icon.py — 规范化应用图标 assets/icon.ico（exe 图标 / 窗口 / 任务栏 / 安装包）
#
# 只做两件事，不重画图形：
#   1. **补全帧集**。原 icon.ico 只有 16/32/48/64/128/256。125% 缩放要 20px、150% 要 24px、
#      175% 要 28px、200% 要 32px —— 缺帧时 Windows 只能拉伸邻近帧，必然发糊。
#      这才是 DPI 相关问题的真正来源。
#   2. 所有帧一律从 256 母版 LANCZOS 重采样（下采样质量优于双三次），小尺寸再叠一层
#      轻锐化抵消重采样带来的软化。
#
# 母版（256 帧）原样保留、不裁不拉伸，因此重复运行是幂等的。
#
# 用法：python assets/make_app_icon.py
# 产出：assets/icon.ico（17 帧）
# 帧集由 assets/check_icons.py 断言（接入 tests/run_tests.ps1）。

from PIL import Image, ImageFilter
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_PATH = os.path.join(REPO, "assets", "icon.ico")

APP_ICON_SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 160, 192, 256]
SHARPEN_BELOW = 36        # 这个尺寸以下再锐化一次（重采样会把小图越缩越软）


def master_frame():
    """取 icon.ico 的最大帧作为母版（整幅，不裁剪）。"""
    im = Image.open(ICON_PATH)
    im.size = max(im.ico.sizes()) if hasattr(im, "ico") else im.size
    return im.convert("RGBA")


def main():
    master = master_frame()
    print("master:", master.size)

    frames = {}
    for s in APP_ICON_SIZES:
        f = master.resize((s, s), Image.LANCZOS)
        if s < SHARPEN_BELOW:
            f = f.filter(ImageFilter.UnsharpMask(radius=0.8, percent=70, threshold=0))
        frames[s] = f

    # 基准图必须是**最大**帧：Pillow 的 ICO 保存会跳过"大于基准图"的槽位，
    # 拿小帧当基准会导致只写出一个尺寸（文件类型图标曾因此踩坑）。
    base = APP_ICON_SIZES[-1]
    frames[base].save(
        ICON_PATH,
        sizes=[(s, s) for s in APP_ICON_SIZES],
        append_images=[frames[s] for s in APP_ICON_SIZES if s != base],
        bitmap_format="png")
    print("wrote", ICON_PATH, "frames:", APP_ICON_SIZES)


if __name__ == "__main__":
    main()
