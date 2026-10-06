# make_format_icons.py — 生成各文件格式的关联图标（PDF/EPUB/MOBI/FB2/CBZ/XPS/IMG）
#
# 版式语言：白纸 + 右上折角 + 底部格式色带（带格式缩写）+ 品牌幽灵。
#
# 三个必须守住的点（都踩过坑）：
#   1. **每个尺寸帧原生绘制**，绝不"画一张 256 再缩放"。做法是"超采样"：
#      在 T×SS 的画布上按比例作画，再 LANCZOS 降到 T×T——这样轮廓/文字天然抗锯齿，
#      而不是 PIL 那种硬边（PIL 的 ImageDraw 不做抗锯齿，直接按目标尺寸画会锯齿/发糊）。
#   2. **色带必须裁到页面轮廓内**。页面底角是圆角，色带若是普通矩形就会在底部两个角
#      "溢出"纸面（用户实测反馈"色块填充溢出了"）。统一用页面 mask 裁剪。
#   3. **帧集要覆盖非整数缩放率**。125% 下 16px 槽位实际是 20 物理像素、150% 是 24、
#      175% 是 28……没有对应帧时 Windows 会拉伸邻近帧，必然发糊。故补 20/24/28/36/40/
#      56/60/72/96/160/192 等帧。
#
# 分级（同一套版式，按尺寸给不同细节量；小像素不加装饰）：
#   16~28  tiny     无幽灵、无色带文字；色带占下半屏，靠"颜色 + 纸形"识别
#   32~64  compact  加幽灵 + 色带文字；细轮廓，无投影
#   72+    standard 加投影 + 纸面渐变 + 色带渐变/高光 + 幽灵投影
#
# 用法：python assets/make_format_icons.py
# 产出：installer/icons/lilith_<EXT>.ico（多尺寸帧，入库）
#       预览图：%TEMP%\lilith-icons\（256 大图、浅/深底接触表、小尺寸矩阵；不入库）
# 依赖：Pillow（pip install pillow）
#
# 帧集完整性由 assets/check_icons.py 断言（接入 tests/run_tests.ps1）。

from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
import os
import math

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GHOST_SRC = os.path.join(REPO, "assets", "icon.ico")
OUT_DIR = os.path.join(REPO, "installer", "icons")
PREVIEW_DIR = os.path.join(os.environ.get("TEMP", r"C:\Temp"), "lilith-icons")

FONT_PATH = r"C:\Windows\Fonts\segoeuib.ttf"     # Segoe UI Bold
FONT_FALLBACK = r"C:\Windows\Fonts\arialbd.ttf"

FORMATS = {
    "PDF":  (229, 57, 53),    # 红
    "EPUB": (67, 160, 71),    # 绿
    "MOBI": (251, 140, 0),    # 橙
    "FB2":  (142, 36, 170),   # 紫
    "CBZ":  (30, 136, 229),   # 蓝
    "XPS":  (0, 137, 123),    # 青
    "IMG":  (0, 172, 193),    # 图片（安装包默认不绑定，仅备用）
}

# 覆盖 100/125/150/175/200% 下常见槽位（16/24/32/48/64/96/128/256 的整数倍缩放）
ICO_SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 160, 192, 256]

# ---- 版式常量（都在 256 坐标系里定义，绘制时按 k 缩放）----
PAGE_MARGIN = 20          # 纸面到画布边缘（留出投影空间）
PAGE_RADIUS = 22
FOLD = 50                 # 折角边长
BAND_TOP = {"tiny": 0.56, "compact": 0.70, "standard": 0.72}   # 色带顶边占纸高比例
GHOST_H = {"tiny": 0.0, "compact": 0.90, "standard": 0.94}     # 幽灵高占纸高比例

PAGE_TOP_RGB = (255, 255, 255)
PAGE_BOT_RGB = (238, 240, 245)
# 纸面轮廓：**浅**灰。深灰在小尺寸下会缩成一圈"脏灰边"（用户实测反馈），
# 浅灰 + 投影才是给出纸面边界的正确手段。
EDGE_RGB = (168, 172, 182)


def tier_for(size):
    if size <= 28:
        return "tiny"
    if size <= 64:
        return "compact"
    return "standard"


def supersample(size):
    """让绘制画布至少约 768px，保证轮廓与文字的抗锯齿质量；上限 32×。"""
    return max(1, min(32, math.ceil(768 / size)))


def mix(c1, c2, t):
    return tuple(round(a + (b - a) * t) for a, b in zip(c1[:3], c2[:3]))


def lighten(c, f):
    return mix(c, (255, 255, 255), f)


def darken(c, f):
    return mix(c, (0, 0, 0), f)


def load_ghost():
    """取 icon.ico 的最大帧并裁到 alpha 包围盒——裁掉透明留白后，摆放坐标才是可预期的。"""
    im = Image.open(GHOST_SRC)
    im.size = max(im.ico.sizes()) if hasattr(im, "ico") else im.size
    im = im.convert("RGBA")
    bb = im.getchannel("A").getbbox()
    return im.crop(bb)


def vgradient(size, top, bottom):
    """竖向线性渐变（RGBA）。"""
    g = Image.new("RGB", (1, size), top[:3])
    px = g.load()
    for y in range(size):
        px[0, y] = mix(top, bottom, y / max(1, size - 1))
    return g.resize((size, size)).convert("RGBA")


def page_mask(S, k, radius, fold):
    """页面轮廓 mask：圆角矩形减去右上折角三角。"""
    m = PAGE_MARGIN * k
    x0, y0, x1, y1 = m, m, S - m, S - m
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([x0, y0, x1, y1], radius=radius, fill=255)
    md.polygon([(x1, y0), (x1, y0 + fold), (x1 - fold, y0)], fill=0)
    return mask


def fit_font(label, max_w, max_h, scale_px):
    """把格式缩写塞进色带：从大到小试，直到宽高都放得下。"""
    path = FONT_PATH if os.path.exists(FONT_PATH) else FONT_FALLBACK
    size = max(6, int(max_h))
    while size > 5:
        f = ImageFont.truetype(path, size)
        bb = f.getbbox(label)
        if (bb[2] - bb[0]) <= max_w and (bb[3] - bb[1]) <= max_h:
            return f, bb
        size -= 1
    f = ImageFont.truetype(path, 6)
    return f, f.getbbox(label)


def draw_icon(T, label, color, ghost, tier):
    """在 T×T 上渲染一帧（内部超采样）。绘制顺序即遮挡关系：
    投影 → 纸面 → 幽灵 → 色带 → 轮廓 → 折角翻片 → 文字。
    幽灵必须早于色带，否则尾巴会盖在色带上（应该被色带压住）。"""
    SS = supersample(T)
    S = T * SS
    k = S / 256.0

    def R(v):
        return v * k

    m = R(PAGE_MARGIN)
    radius = R(PAGE_RADIUS)
    fold = R(FOLD)
    x0, y0, x1, y1 = m, m, S - m, S - m
    page_w = x1 - x0
    page_h = y1 - y0

    # 轮廓线宽：按比例走，但小尺寸给下限，否则纸面在白底上"消失"。
    # tiny 用更浅的灰 + 更细的线：16px 下 1px 线已占图标宽 6%，深灰会缩成一圈"脏灰边"。
    if tier == "tiny":
        edge_col = mix(EDGE_RGB, (255, 255, 255), 0.28)
        ow = max(0.8 * SS, R(1.6))
    else:
        edge_col = EDGE_RGB
        ow = max(0.9 * SS, R(3.2))
    ow_px = max(1, int(round(ow)))

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    mask = page_mask(S, k, radius, fold)

    # ---- 1. 投影（仅 standard；小尺寸加投影只会糊成一团）----
    if tier == "standard":
        sh = mask.filter(ImageFilter.GaussianBlur(radius=R(7)))
        sh = ImageChops.offset(sh, 0, int(R(6)))
        sh = ImageChops.multiply(sh, Image.new("L", (S, S), 92))
        shadow = Image.new("RGBA", (S, S), (44, 48, 60, 255))
        shadow.putalpha(sh)
        img.alpha_composite(shadow)

    # ---- 2. 纸面（standard 带极轻渐变，其余纯白）----
    body = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if tier == "standard":
        body.alpha_composite(vgradient(S, PAGE_TOP_RGB, PAGE_BOT_RGB))
    else:
        ImageDraw.Draw(body).rectangle([0, 0, S, S], fill=PAGE_TOP_RGB + (255,))

    # ---- 3. 幽灵（tiny 不放：几像素的幽灵只会是灰点）----
    if GHOST_H[tier] > 0:
        gh = page_h * GHOST_H[tier]
        gw = gh * ghost.width / ghost.height
        g = ghost.resize((max(1, int(round(gw))), max(1, int(round(gh)))), Image.LANCZOS)
        gx = x0 + (page_w - g.width) / 2
        gy = y0 + R(7)
        if tier == "standard":
            gm = Image.new("L", (S, S), 0)
            gm.paste(g.getchannel("A"), (int(gx + R(4)), int(gy + R(6))))
            gm = ImageChops.multiply(gm.filter(ImageFilter.GaussianBlur(radius=R(3))),
                                     Image.new("L", (S, S), 120))
            gs = Image.new("RGBA", (S, S), (70, 74, 92, 255))
            gs.putalpha(ImageChops.multiply(gm, mask))
            body.alpha_composite(gs)
        body.alpha_composite(g, (int(round(gx)), int(round(gy))))

    # ---- 4. 格式色带（裁到页面轮廓内 —— 这是"色块溢出"的修法）----
    band = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bdr = ImageDraw.Draw(band)
    by = y0 + page_h * BAND_TOP[tier]
    if tier == "standard":
        bdr.rectangle([0, by, S, S], fill=darken(color, 0.10) + (255,))
        bdr.rectangle([0, by, S, by + (y1 - by) * 0.55], fill=color + (255,))
    else:
        bdr.rectangle([0, by, S, S], fill=color + (255,))
    # 色带顶边高光：给"色块"一点厚度感（tiny 不加，一像素的高光只会变脏）
    if tier != "tiny":
        bdr.rectangle([x0, by, x1, by + max(1.0, R(2.4))], fill=lighten(color, 0.34) + (255,))
    band.putalpha(ImageChops.multiply(band.getchannel("A"), mask))
    body.alpha_composite(band)

    # ---- 5. 纸面轮廓（裁到轮廓内，避免折角处残留圆弧）----
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ed = ImageDraw.Draw(edge)
    ed.rounded_rectangle([x0, y0, x1, y1], radius=radius, outline=edge_col + (255,), width=ow_px)
    ed.line([(x1 - fold, y0), (x1, y0 + fold)], fill=edge_col + (255,), width=ow_px)
    body.alpha_composite(edge)

    # ---- 6. 一次性裁到纸面轮廓 ----
    # 纸面渐变/纯白是**整块画布**填充的，不裁的话图标就是一张不透明白底方块 ——
    # Explorer 里选中（蓝色高亮）时会露出白底。这里裁掉的是"纸以外"的部分，
    # 纸面本身仍是实心不透明的（不要把图标做成空心）。
    body.putalpha(ImageChops.multiply(body.getchannel("A"), mask))

    # ---- 7. 折角翻片（画在裁剪之外：它本来就是"翻起来的那片"）----
    fd = ImageDraw.Draw(body)
    fold_col = lighten(color, 0.55) if tier == "standard" else lighten(color, 0.35)
    fd.polygon([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=fold_col + (255,))
    if tier != "tiny":
        fd.line([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)],
                fill=edge_col + (255,), width=ow_px)

    # ---- 8. 格式缩写 ----
    if tier != "tiny":
        band_h = y1 - by
        fnt, bb = fit_font(label, page_w * 0.82, band_h * 0.60, R(52))
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        fd.text((x0 + (page_w - tw) / 2 - bb[0], by + (band_h - th) / 2 - bb[1]),
                label, font=fnt, fill=(255, 255, 255, 255))

    img.alpha_composite(body)
    return img.resize((T, T), Image.LANCZOS)


def build_format(label, color, ghost):
    frames = {s: draw_icon(s, label, color, ghost, tier_for(s)) for s in ICO_SIZES}
    # 关键：基准图必须是**最大**帧。Pillow 的 ICO 保存对 "size > 基准图尺寸" 的槽位直接
    # continue（IcoImagePlugin._save），若拿 16px 当基准，最终只会写出 16×16 一帧 —— 正是
    # "Explorer 里图标发糊 + 一圈灰边"的根因。以 256 为基准、其余走 append_images，
    # Pillow 才能为每个槽位取到尺寸精确匹配的原生帧。
    base = ICO_SIZES[-1]
    frames[base].save(
        os.path.join(OUT_DIR, f"lilith_{label}.ico"),
        sizes=[(s, s) for s in ICO_SIZES],
        append_images=[frames[s] for s in ICO_SIZES if s != base],
        bitmap_format="png")
    frames[256].save(os.path.join(PREVIEW_DIR, f"lilith_{label}_256.png"))
    return frames


def contact_sheet(icons, bg, path, dark=False, cell=256, caption_h=40):
    n = len(icons)
    s = Image.new("RGB", (cell * n, cell + caption_h), bg)
    dd = ImageDraw.Draw(s)
    fnt = ImageFont.truetype(FONT_PATH if os.path.exists(FONT_PATH) else FONT_FALLBACK, 24)
    for i, (label, im) in enumerate(icons.items()):
        tile = Image.new("RGBA", (cell, cell + caption_h), (0, 0, 0, 0))
        tile.alpha_composite(im.resize((cell, cell), Image.LANCZOS), (0, 0))
        s.paste(tile, (i * cell, 0), tile)
        text = "." + label.lower()
        bb = dd.textbbox((0, 0), text, font=fnt)
        dd.text((i * cell + (cell - bb[2] + bb[0]) / 2, cell + 6), text, font=fnt,
                fill=(205, 205, 212) if dark else (58, 58, 58))
    s.save(path)


def size_matrix(ghost, sizes, bg, path, cell=120, zoom=6):
    """把若干目标尺寸放大 NEAREST 摆一排：用来肉眼确认"小尺寸是否清晰"。"""
    s = Image.new("RGB", (cell * len(sizes), cell), bg)
    dd = ImageDraw.Draw(s)
    fnt = ImageFont.truetype(FONT_PATH if os.path.exists(FONT_PATH) else FONT_FALLBACK, 15)
    for i, t in enumerate(sizes):
        im = draw_icon(t, "PDF", FORMATS["PDF"], ghost, tier_for(t))
        big = im.resize((t * zoom, t * zoom), Image.NEAREST)
        big.thumbnail((cell - 10, cell - 34), Image.NEAREST)
        s.paste(big, (i * cell + 5, 5), big)
        dd.text((i * cell + 6, cell - 24), f"{t}px", font=fnt, fill=(90, 90, 90))
    s.save(path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    ghost = load_ghost()
    print("ghost sprite:", ghost.size)

    frames = {}
    for label, color in FORMATS.items():
        frames[label] = build_format(label, color, ghost)
        print("built", label)

    big = {l: draw_icon(256, l, c, ghost, "standard") for l, c in FORMATS.items()}
    contact_sheet(big, (240, 240, 242), os.path.join(PREVIEW_DIR, "preview_light.png"))
    contact_sheet(big, (38, 38, 42), os.path.join(PREVIEW_DIR, "preview_dark.png"), dark=True)
    size_matrix(ghost, [16, 20, 24, 32, 40, 48], (240, 240, 242),
                os.path.join(PREVIEW_DIR, "preview_small.png"))
    print("previews ->", PREVIEW_DIR)


if __name__ == "__main__":
    main()
