#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构造 tests/samples/ 下缺失的回归样本（不依赖第三方库）。

产出：
  real_multichapter.epub —— 多章节、每章有实质正文的 EPUB（供 12-9 跨页选区持久性验证）。
                            正文**混排中文 / 拉丁扩展 / 西里尔 / 希腊 / 符号**，用来观察不同
                            字形的行框高度与行距差异对选区 / 搜索高亮的影响。

注意：
  - empty.pdf / truncated.pdf / random.pdf / mobi_huffcdic.mobi 已存在，无需重建。
  - 「单页渲染失败」的静态 PDF 造不出来：实测 MuPDF 对内容流 / 资源 / MediaBox 的破坏都只记
    日志、不抛异常，故本脚本不再生成 corrupt_page.pdf（曾造出的那个渲染成空白页，会误导）。
  - 16-1（落盘失败）不是文件样本，是运行期权限条件，见同目录 make_state_readonly.ps1。
"""

import os
import zipfile

# 生成物统一落在 tests/samples/ 下
SAMPLES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "samples")


# --------------------------------------------------------------------------- #
# 多章节 EPUB（混排多语种文本）
# --------------------------------------------------------------------------- #
# 英文正文（拉丁基础字形）
_EN = [
    "Lilith Reader is a desktop document reader built for people who actually read.",
    "The page canvas is implemented from scratch so that zooming never blurs the text.",
    "Reading position, zoom, columns, rotation and paper scheme are remembered per book.",
    "The identity of a book is a content fingerprint, not the file path on disk.",
    "A document is parsed on a background thread so the interface never blocks on disk.",
    "Text selection and copy require a real text layer; scanned pages have none.",
    "Full text search debounces for about four hundred milliseconds before it runs.",
    "The user interface follows the system DPI so high resolution screens stay sharp.",
    "Bookmarks are saved alongside the document and survive a file move or re-download.",
    "The reader keeps a previous frame on screen while the high resolution render lands.",
]

# 中文（CJK 字框≈整 em，行距通常更大，用来对比高亮贴合）
_ZH = [
    "莉莉丝阅读器是一款为真正阅读的人设计的桌面文档阅读器，缩放时文字始终保持清晰。",
    "阅读位置、缩放比例、分栏、旋转与纸张方案会按书分别记住，与文件的存放路径无关。",
    "文档在后台线程解析，界面永远不会因为磁盘读取而卡住；选中文字与复制需要真实的文本层。",
    "全文检索大约在停止输入四百毫秒后开始；书签随文档保存，移动文件或重新下载都不会丢失。",
]

# 拉丁扩展 / 西里尔 / 希腊（带升降部与重音，用来暴露行框高度差）
_EU = [
    "Le lecteur garde la position, le niveau de zoom et la mise en page pour chaque livre.",
    "Die Oberfläche skaliert mit der System-DPI, damit Text auf hochauflösenden Bildschirmen scharf bleibt.",
    "Читальное положение, масштаб и разметка запоминаются отдельно для каждой книги.",
    "Η θέση ανάγνωσης και η διάταξη αποθηκεύονται ξεχωριστά για κάθε βιβλίο.",
]

# 纯符号 / 标点 / 数字行（测试极端字形）
_SYM = "①②③ ④⑤⑥ ★ ☆ ☞ — – “ ” ‘ ’ … § ¶ ÆŒßÞð 0123456789"


def _chapter_body(title: str, seed: int, paras: int) -> str:
    """生成若干段正文（英 / 中 / 多语种混排），保证每章有足够长度跨多页。"""
    out = [f"<h1>{title}</h1>"]
    for p in range(paras):
        base = seed * 7 + p * 3
        if p % 4 == 1:
            line = "".join(_ZH[(base + i) % len(_ZH)] for i in range(3))
        elif p % 4 == 3:
            line = " ".join(_EU[(base + i) % len(_EU)] for i in range(3))
        else:
            line = " ".join(_EN[(base + i) % len(_EN)] for i in range(6))
        out.append(f"<p>{line}</p>")
    out.append(f"<p>{_SYM}</p>")
    return "\n".join(out)


def build_epub(path: str) -> None:
    chapters = [
        ("Chapter One", 1, 14),
        ("Chapter Two", 2, 14),
        ("Chapter Three", 3, 14),
    ]
    # OPF
    opf = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:lilith-multichapter-sample</dc:identifier>
    <dc:title>Lilith Multichapter Sample</dc:title>
    <dc:language>zh-CN</dc:language>
    <dc:creator>Lilith Reader</dc:creator>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="c1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="c2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
    <item id="c3" href="chapter3.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="c1"/>
    <itemref idref="c2"/>
    <itemref idref="c3"/>
  </spine>
</package>
"""
    ncx = """<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="urn:uuid:lilith-multichapter-sample"/>
  </head>
  <docTitle><text>Lilith Multichapter Sample</text></docTitle>
  <navMap>
    <navPoint id="n1" playOrder="1"><navLabel><text>Chapter One</text></navLabel><content src="chapter1.xhtml"/></navPoint>
    <navPoint id="n2" playOrder="2"><navLabel><text>Chapter Two</text></navLabel><content src="chapter2.xhtml"/></navPoint>
    <navPoint id="n3" playOrder="3"><navLabel><text>Chapter Three</text></navLabel><content src="chapter3.xhtml"/></navPoint>
  </navMap>
</ncx>
"""
    xhtml_tmpl = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><meta charset="utf-8"/><title>{t}</title></head>
<body>
{b}
</body>
</html>
"""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        # mimetype 必须是第一个条目且以 STORED 存放
        z.writestr(zipfile.ZipInfo("mimetype", date_time=(1980, 1, 1, 0, 0, 0)),
                   "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>""")
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/toc.ncx", ncx)
        for i, (title, seed, paras) in enumerate(chapters, start=1):
            z.writestr(f"OEBPS/chapter{i}.xhtml",
                       xhtml_tmpl.format(t=title, b=_chapter_body(title, seed, paras)))
    print(f"[ok] {os.path.basename(path)}  ({os.path.getsize(path)} B)")


def main() -> None:
    build_epub(os.path.join(SAMPLES_DIR, "real_multichapter.epub"))


if __name__ == "__main__":
    main()
