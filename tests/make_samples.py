# make_samples.py — 生成自动化测试样本集（tests/samples/，生成物不入库）
#
# 覆盖四类：
#   1. 合法文件：PDF / EPUB / FB2 / XPS / PNG / CBZ
#   2. 改名可读：图片、epub、xps、fb2 改名成别的受支持扩展名（应打开，仅提示不符）
#   3. 归档冒充：zip 图集/文档集改名 .epub/.pdf/.fb2/.cbz（应拒绝，ADR-016）
#   4. 加密与损坏：AES 加密 PDF、截断、空文件、纯文本改名、需修复的 PDF
#   5. 压力样本：1200 页 / A0 幅面；超大页：触发 app 渲染前像素闸门 → 页级渲染失败（ADR-100）
#
# 加密样本依赖 PyMuPDF（anaconda 自带，import fitz）。缺库时**跳过加密样本**并提示，
# 其余样本照常生成 —— doc_test.cpp 对缺失的加密样本报 SKIP 而不是 FAIL。
#
# 用法： python make_samples.py [输出目录，默认 tests/samples]

import io
import os
import random
import struct
import sys
import zlib
import zipfile

RANDOM_SEED = 20261005  # 固定种子：损坏类样本须可复现
USER_PW = "lilith"      # 加密样本的 user password（doc_test.cpp 用它验证 authenticate）
OWNER_PW = "owner"


# ---------------------------------------------------------------- 基础构造

def minimal_pdf(pages=1, mediabox="0 0 200 300", mediaboxes=None, contents=None):
    """结构合法、xref 正确的多页最小 PDF。

    mediaboxes 给出时按页指定 MediaBox（用于构造**异构页尺寸**样本）；
    否则所有页用同一个 mediabox。
    contents 给出时按页指定内容流（bytes）；否则每页写一句 "page"。
    两者都是为了构造"只有具体几何/内容才成立"的边界样本。

    对象编号约定（务必与下面的 append 顺序一致）：
      1 Catalog / 2 Pages / 3 Resources / 4 Font
      之后每页占两个对象：Page = 5+2i，Contents 流 = 6+2i
    """
    if mediaboxes is not None:
        assert len(mediaboxes) == pages
    if contents is not None:
        assert len(contents) == pages
    objs = []
    kids = " ".join(f"{5 + 2 * i} 0 R" for i in range(pages))
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objs.append(f"<< /Type /Pages /Kids [ {kids} ] /Count {pages} >>".encode())
    objs.append(b"<< /Font << /F1 3 0 R >> >>")
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    for i in range(pages):
        mb = mediaboxes[i] if mediaboxes is not None else mediabox
        objs.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox [ {mb} ] "
            f"/Resources 3 0 R /Contents {6 + 2 * i} 0 R >>".encode()
        )
        stream = contents[i] if contents is not None else b"BT /F1 24 Tf 20 150 Td (page) Tj ET"
        objs.append(b"<< /Length " + str(len(stream)).encode()
                    + b" >>\nstream\n" + stream + b"\nendstream")

    out = io.BytesIO()
    out.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, start=1):
        offsets.append(out.tell())
        out.write(f"{i} 0 obj\n".encode() + body + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(f"xref\n0 {n}\n".encode())
    out.write(b"0000000000 65535 f \n")
    for off in offsets:
        out.write(f"{off:010d} 00000 n \n".encode())
    out.write(f"trailer\n<< /Size {n} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    return out.getvalue()


# ---- 超大页的内容流（页级渲染失败样本，见 main() 的「8.」一节）----------------
#
# 为什么内容要**看得见**：这两页的用途是人工判断"渲染成功 / 保留旧纹理被拉伸变糊"。
# 纯白页看不出任何差别（曾用纯白方形页，人工反馈"始终是白色，没什么特别之处"）；
# 竖长页曾把文字放在最底部 —— 视口在页面顶部，渲染成功后也看不到，同样白费。

TALL_W, TALL_H = 200, 600000        # 竖长页：fit-width 下输出高约 6.2e6 px，远超像素上限
SQUARE_SIDE = 20000                 # 方形页：fit-width 正常，放大到约 75% 以上才超限


def tall_page_content():
    """竖长页内容：顶部蓝条 + 红条 + 文字（缩放恢复后应能看到这三样）。"""
    return b"\n".join([
        b"q 0 0 1 rg 0 %d %d 100 re f Q" % (TALL_H - 100, TALL_W),
        b"q 1 0 0 rg 0 %d %d 100 re f Q" % (TALL_H - 300, TALL_W),
        b"BT /F1 60 Tf 0 0 0 rg 20 %d Td (TOP OF TALL PAGE) Tj ET" % (TALL_H - 480),
    ])


def square_page_content():
    """方形大页内容：边框 + 网格 + 红块 + 大字（放大超限后应整体变糊）。"""
    s = SQUARE_SIDE
    parts = [b"q 0 0 0 RG 60 w 100 100 %d %d re S Q" % (s - 200, s - 200),
             b"q 0 0 0 RG 25 w"]
    for i in range(1, s // 2000):
        c = i * 2000
        parts.append(b" %d 100 m %d %d l S" % (c, c, s - 100))
        parts.append(b" 100 %d m %d %d l S" % (c, s - 100, c))
    parts.append(b"Q")
    parts.append(b"q 1 0 0 rg 12000 12000 4000 4000 re f Q")
    parts.append(b"BT /F1 1400 Tf 0 0 0 rg 700 17200 Td (BIG PAGE 20000x20000) Tj ET")
    return b"\n".join(parts)


def pdf_with_outline(pages=3):
    """带两级目录（outline）的最小 PDF，供 doc_test 断言 outline() 解析。

    对象编号：
      1 Catalog / 2 Pages / 3 Resources / 4 Font
      之后每页两个对象：Page = 5+2i，Contents = 6+2i
      目录对象：root = n_base+1，item1 = n_base+2，child1 = n_base+3，item2 = n_base+4
    目录结构（前序）：Chapter 1（→页0，含子项） / Section 1.1（→页1） / Chapter 2（→页2）
    """
    assert pages >= 2
    page_obj = [5 + 2 * i for i in range(pages)]
    n_base = 4 + 2 * pages
    root_no, item1_no, child1_no, item2_no = n_base + 1, n_base + 2, n_base + 3, n_base + 4

    objs = []
    kids = " ".join(f"{page_obj[i]} 0 R" for i in range(pages))
    objs.append(f"<< /Type /Catalog /Pages 2 0 R /Outlines {root_no} 0 R >>".encode())
    objs.append(f"<< /Type /Pages /Kids [ {kids} ] /Count {pages} >>".encode())
    objs.append(b"<< /Font << /F1 4 0 R >> >>")
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    for i in range(pages):
        objs.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 300 ] "
            f"/Resources 3 0 R /Contents {6 + 2 * i} 0 R >>".encode()
        )
        stream = b"BT /F1 24 Tf 20 150 Td (page) Tj ET"
        objs.append(b"<< /Length " + str(len(stream)).encode()
                    + b" >>\nstream\n" + stream + b"\nendstream")
    # 目录对象（ASCII 标题，避免断言里的编码问题）
    objs.append(f"<< /Type /Outlines /First {item1_no} 0 R /Last {item2_no} 0 R "
                f"/Count 3 >>".encode())
    objs.append(
        f"<< /Title (Chapter 1) /Parent {root_no} 0 R /Next {item2_no} 0 R "
        f"/First {child1_no} 0 R /Last {child1_no} 0 R /Count 2 "
        f"/Dest [ {page_obj[0]} 0 R /Fit ] >>".encode()
    )
    objs.append(
        f"<< /Title (Section 1.1) /Parent {item1_no} 0 R "
        f"/Dest [ {page_obj[1]} 0 R /Fit ] >>".encode()
    )
    objs.append(
        f"<< /Title (Chapter 2) /Parent {root_no} 0 R /Prev {item1_no} 0 R "
        f"/Dest [ {page_obj[2]} 0 R /Fit ] >>".encode()
    )

    out = io.BytesIO()
    out.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, start=1):
        offsets.append(out.tell())
        out.write(f"{i} 0 obj\n".encode() + body + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(f"xref\n0 {n}\n".encode())
    out.write(b"0000000000 65535 f \n")
    for off in offsets:
        out.write(f"{off:010d} 00000 n \n".encode())
    out.write(f"trailer\n<< /Size {n} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    return out.getvalue()


def png_1x1(gray=200):
    """最小合法 PNG（1x1 RGB）。"""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    raw = bytes([0, gray, gray, 255])
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def zip_of(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(name, data)
    return buf.getvalue()


def make_epub():
    """结构合法的最小 EPUB（mimetype + container.xml + OPF + xhtml）。"""
    return zip_of([
        ("mimetype", "application/epub+zip"),
        ("META-INF/container.xml", """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf"
    media-type="application/oebps-package+xml"/></rootfiles>
</container>"""),
        ("OEBPS/content.opf", """<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Test Book</dc:title><dc:identifier id="id">test-1</dc:identifier>
    <dc:language>zh</dc:language>
  </metadata>
  <manifest><item id="c1" href="chapter1.xhtml" media-type="application/xhtml+xml"/></manifest>
  <spine><itemref idref="c1"/></spine>
</package>"""),
        ("OEBPS/chapter1.xhtml", """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>c1</title></head>
<body><h1>第一章</h1><p>测试正文。</p></body></html>"""),
    ])


def make_xps():
    """结构合法的最小 XPS。"""
    return zip_of([
        ("[Content_Types].xml", b"""<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="fdseq" ContentType="application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>
<Default Extension="fdoc" ContentType="application/vnd.ms-package.xps-fixeddocument+xml"/>
<Default Extension="fpage" ContentType="application/vnd.ms-package.xps-fixedpage+xml"/>
</Types>"""),
        ("_rels/.rels", b"""<?xml version="1.0" encoding="utf-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="R1" Type="http://schemas.microsoft.com/xps/2005/06/fixedrepresentation"
 Target="/FixedDocSeq.fdseq"/>
</Relationships>"""),
        ("FixedDocSeq.fdseq", b"""<?xml version="1.0" encoding="utf-8"?>
<FixedDocumentSequence xmlns="http://schemas.microsoft.com/xps/2005/06">
<DocumentReference Source="/Documents/1/FixedDoc.fdoc"/>
</FixedDocumentSequence>"""),
        ("Documents/1/FixedDoc.fdoc", b"""<?xml version="1.0" encoding="utf-8"?>
<FixedDocument xmlns="http://schemas.microsoft.com/xps/2005/06">
<PageContent Source="/Documents/1/Pages/1.fpage"/>
</FixedDocument>"""),
        ("Documents/1/Pages/1.fpage", b"""<?xml version="1.0" encoding="utf-8"?>
<FixedPage xmlns="http://schemas.microsoft.com/xps/2005/06" Width="816" Height="1056"
 xml:lang="zh-CN"><Path Fill="#000000" Data="M 100 100 L 300 100 L 300 200 Z"/></FixedPage>"""),
    ])


def minimal_mobi(text, compression=1, name=b"LilithMobiSample"):
    """最小合法 MOBI（Palm Database 容器，type/creator = TEXtREAd，即 PalmDOC）。

    MuPDF 的 MOBI 处理器只实现无压缩（1）与 PalmDOC（2）两种压缩；HUFF/CDIC（17480）
    会以"unknown compression method"拒绝。故本生成器用 compression 参数覆盖两条支持路径，
    并用 17480 造一个"格式合法但压缩不受支持"的边界样本。

    布局（参见 source/html/mobi.c 的 fz_extract_html_from_mobi）：
      0   : 32  数据库名（NUL 填充）
      32  : 28  attributes/version/日期/appInfoID/sortInfoID
      60  : 8   type + creator = "TEXtREAd"
      68  : 8   uniqueIDseed + nextRecordListID
      76  : 2   记录数 n
      78  : 8n  记录信息表（每条：偏移 4 + 属性 4，均大端）
      随后     记录 0 = 16 字节 PalmDOC 头，记录 1 = 正文

    TEXtREAd 为纯文本格式，正文按 Latin-1 直接放置（ASCII 文本同时是合法的 PalmDOC
    字面量序列，故 compression=2 时无需重编码）。
    """
    raw = text.encode("latin-1", "replace") if isinstance(text, str) else text
    # PalmDOC 头：compression / unused / text_length / record_count / record_size / 加密 / 保留
    rec0 = struct.pack(">HHIHHHH", compression, 0, len(raw), 1, 4096, 0, 0)
    records = [rec0, raw]

    head = name[:31].ljust(32, b"\0")
    # 28 字节：attributes / version / 三个日期 / modificationNumber / appInfoID / sortInfoID
    head += struct.pack(">HHIIIIII", 0, 0, 0, 0, 0, 0, 0, 0)
    head += b"TEXtREAd"
    head += struct.pack(">II", 0, 0)                        # uniqueIDseed, nextRecordListID
    n = len(records)
    head += struct.pack(">H", n)

    offsets, off = [], len(head) + n * 8
    for r in records:
        offsets.append(off)
        off += len(r)
    info = b"".join(struct.pack(">II", o, 0) for o in offsets)
    return head + info + b"".join(records)


FB2 = """<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description>
  <title-info><genre>sf</genre><author><nickname>test</nickname></author>
    <book-title>Test FB2</book-title><lang>zh</lang></title-info>
  <document-info><author><nickname>test</nickname></author>
    <program-used>tests</program-used><date value="2026-01-01">2026</date>
    <id>test-1</id><version>1.0</version></document-info>
</description>
<body><section><title><p>第一章</p></title><p>测试正文。</p></section></body>
</FictionBook>
""".encode("utf-8")


# ---------------------------------------------------------------- 加密样本

def make_encrypted(out_dir, log):
    """用 PyMuPDF 生成加密 PDF。缺库则跳过（不生成文件）。"""
    try:
        import fitz  # PyMuPDF
    except Exception:
        log("  ! 未安装 PyMuPDF（import fitz 失败）→ 跳过 3 个加密样本")
        return

    ver = "?".join(str(x) for x in getattr(fitz, "version", ("?",)))
    log(f"  （PyMuPDF {ver}）")

    def save(name, **kw):
        doc = fitz.open()
        page = doc.new_page()
        page.insert_text((72, 120), "encrypted sample", fontsize=18)
        path = os.path.join(out_dir, name)
        doc.save(path, **kw)
        doc.close()
        log(f"  {name:28s} {os.path.getsize(path):>8d} B")

    save("enc_aes128_user.pdf", encryption=fitz.PDF_ENCRYPT_AES_128,
         user_pw=USER_PW, owner_pw=OWNER_PW)
    save("enc_aes256_user.pdf", encryption=fitz.PDF_ENCRYPT_AES_256,
         user_pw=USER_PW, owner_pw=OWNER_PW)
    # 只设 owner 密码：PDF 规范允许直接打开（权限受限），MuPDF 不应报需要密码
    save("enc_owner_only.pdf", encryption=fitz.PDF_ENCRYPT_AES_128,
         user_pw="", owner_pw=OWNER_PW)


# ---------------------------------------------------------------- 主流程

def build_pdf(objs):
    """把对象体列表（1 基编号）拼成合法 PDF（xref 正确）。"""
    out = io.BytesIO()
    out.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, start=1):
        offsets.append(out.tell())
        out.write(f"{i} 0 obj\n".encode() + body + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(f"xref\n0 {n}\n".encode())
    out.write(b"0000000000 65535 f \n")
    for off in offsets:
        out.write(f"{off:010d} 00000 n \n".encode())
    out.write(f"trailer\n<< /Size {n} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    return out.getvalue()


def _image_xobj(rgb, size=8):
    """size×size 的 DeviceRGB 图像 XObject 体（RGB 为 (r,g,b) 单色填充）。"""
    data = bytes(rgb) * (size * size)
    return (f"<< /Type /XObject /Subtype /Image /Width {size} /Height {size} "
            f"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length {len(data)} >>\n"
            f"stream\n").encode() + data + b"\nendstream"


def pdf_image_and_text():
    """一页：顶部文字 + 下半部分**纯红色照片**。

    供 doc_test 断言"配色只作用于纸墨层"：深色纸张下纸面变深、而红图必须仍是红的
    （若被 LUT 一起变换，红会变成青蓝色调）。
    """
    content = (b"BT /F1 24 Tf 20 220 Td (page) Tj ET\n"
               b"q 200 0 0 100 0 0 cm /Im1 Do Q")
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [ 5 0 R ] /Count 1 >>",
        b"<< /Font << /F1 4 0 R >> /XObject << /Im1 7 0 R >> >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 300 ] "
        b"/Resources 3 0 R /Contents 6 0 R >>",
        b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
        _image_xobj((255, 0, 0)),
    ]
    return build_pdf(objs)


def pdf_scan_only():
    """整页就是一张图（模拟扫描书）：没有文字、图像铺满整页。

    供 doc_test 断言"整页扫描件回退"：深色纸张下整页（含图）必须变深，
    否则扫描书开了深色模式仍是一整页白。
    """
    content = b"q 200 0 0 300 0 0 cm /Im1 Do Q"
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [ 5 0 R ] /Count 1 >>",
        b"<< /Font << /F1 4 0 R >> /XObject << /Im1 7 0 R >> >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 300 ] "
        b"/Resources 3 0 R /Contents 6 0 R >>",
        b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
        _image_xobj((255, 255, 255)),
    ]
    return build_pdf(objs)


def pdf_ink_black():
    """页面中央一块**纯黑**矩形，其余是白纸（无文字、无图像）。

    供 doc_test 断言深色 LUT 的**墨色端点**：纯黑是"最深的墨"，映射后就是 LUT 亮端；
    它与纸面（白底映射）的暖度必须一致 —— 两端点逐通道等斜率是"整页暖度统一"的前提。
    否则越亮的那一端越暖（正文会比纸面黄），而这在只看纸面的断言里完全看不出来。
    """
    # PDF 坐标 y 轴朝上：矩形 (40,100)-(160,200) ⇒ 设备坐标 y 反向后仍是 100..200
    content = b"q 0 0 0 rg 40 100 120 100 re f Q"
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [ 5 0 R ] /Count 1 >>",
        b"<< /Font << /F1 4 0 R >> >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 300 ] "
        b"/Resources 3 0 R /Contents 6 0 R >>",
        b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"\nendstream",
    ]
    return build_pdf(objs)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "samples")
    os.makedirs(out_dir, exist_ok=True)

    lines = []

    def log(s):
        print(s)
        lines.append(s)

    def write(name, data):
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(data)
        log(f"  {name:28s} {len(data):>8d} B")

    pdf1, pdf3 = minimal_pdf(1), minimal_pdf(3)
    png = png_1x1(200)
    txt = "这是一个纯文本文件，不是 PDF。\n".encode("utf-8") * 20
    epub, xps = make_epub(), make_xps()

    log("== 1. 合法文件 ==")
    write("real.pdf", pdf3)
    # 异构页尺寸：PDF 允许各页 MediaBox 不同（封面/插页/横向页/扫描裁切不一）。
    # 画布若只用首页尺寸统一布局，第 2、3 页会被拉伸成首页纵横比 → 形变。
    # 尺寸刻意取三种不同纵横比（Letter 竖版 / 横版 / 3:2），便于断言逐页尺寸。
    write("mixed_size.pdf", minimal_pdf(4, mediaboxes=[
        "0 0 612 792", "0 0 792 612", "0 0 400 600", "0 0 612 792"]))
    write("real.epub", epub)
    write("real.fb2", FB2)
    write("real.xps", xps)
    write("real.png", png)
    # MOBI：此前"声明支持、未充分验证"。这里造两个**格式合法**的 TEXtREAd（PalmDOC）
    # 样本，覆盖 MuPDF 实现的两种压缩（无压缩 / PalmDOC），把"是否真能打开"钉进断言。
    write("real.mobi", minimal_mobi("Lilith Reader MOBI sample.\nSecond line of text.\n", 1))
    write("real_palmdoc.mobi", minimal_mobi("Lilith Reader PalmDOC sample.\n", 2))
    write("comic.cbz", zip_of([("1.png", png), ("2.png", png_1x1(120)), ("3.png", png_1x1(60))]))
    # 带两级目录的 PDF：供 doc_test 断言 outline() 的层级/页号解析
    write("outline.pdf", pdf_with_outline(3))
    # 配色分层渲染的两个样本（ADR-067）：
    #   with_image.pdf 文字 + 红图 → 断言"深色纸张下照片不变色"；
    #   scan_only.pdf  整页一张图   → 断言"整页扫描件整体回退变换"；
    #   ink_black.pdf  一块纯黑     → 断言"墨色与纸面暖度一致"（LUT 两端等斜率）。
    write("with_image.pdf", pdf_image_and_text())
    write("scan_only.pdf", pdf_scan_only())
    write("ink_black.pdf", pdf_ink_black())

    log("== 2. 改名但内容可正常读（应打开，界面提示不符） ==")
    write("img_named_pdf.pdf", png)
    write("img_named_epub.epub", png)
    write("img_named_cbz.cbz", png)
    write("epub_named_pdf.pdf", epub)
    write("xps_named_epub.epub", xps)
    write("fb2_named_epub.epub", FB2)
    write("pdf_named_cbz.cbz", pdf1)

    log("== 3. 归档冒充单文档（应拒绝：Mismatched） ==")
    write("zipimg_named_epub.epub", zip_of([("1.png", png), ("2.png", png_1x1(120))]))
    write("zipimg_named_pdf.pdf", zip_of([("1.png", png), ("2.png", png_1x1(120))]))
    write("zipimg_named_fb2.fb2", zip_of([("1.png", png), ("2.png", png_1x1(120))]))
    write("zippdf_named_cbz.cbz", zip_of([("a.pdf", pdf1), ("b.pdf", pdf1)]))
    write("ziptxt_named_cbz.cbz", zip_of([("a.txt", txt), ("b.txt", txt)]))

    log("== 4. 认不出 / 损坏（MuPDF 也认不出 → Corrupt） ==")
    write("empty.pdf", b"")
    write("truncated.pdf", pdf3[: len(pdf3) // 3])
    write("txt_named_pdf.pdf", txt)
    rnd = random.Random(RANDOM_SEED)
    write("random.pdf", bytes(rnd.randrange(256) for _ in range(4096)))
    write("empty_zip.epub", zip_of([]))
    write("zippdf_named_epub.epub", zip_of([("a.pdf", pdf1), ("b.pdf", pdf1)]))
    # MOBI 边界：Kindle 常见的 HUFF/CDIC 压缩（17480）MuPDF 未实现，
    # 格式合法的 PDB 也会被拒（Corrupt）。用样本把"声明支持 MOBI"的真实边界钉住。
    write("mobi_huffcdic.mobi", minimal_mobi(b"\x00" * 32, 17480))

    log("== 5. 需修复但应能打开（容错用例） ==")
    write("junk_before_header.pdf", b"JUNK" * 64 + pdf3)
    write("no_startxref.pdf", pdf3.replace(b"startxref", b"startXref"))

    log("== 6. 加密 PDF（需要 PyMuPDF） ==")
    make_encrypted(out_dir, log)

    # 压力样本（Phase 7，ADR-079）：规模与幅面的极端值。
    # 1000+ 页：压页表规模、逐页尺寸探测与检索扫描；
    # A0 幅面：841×1189mm = 2384×3370pt，压"超大页 tile 渲染"路径（超过 tile_size_px 才分块）。
    log("== 7. 压力样本（Phase 7） ==")
    write("stress_1200p.pdf", minimal_pdf(pages=1200))
    write("poster_a0.pdf", minimal_pdf(pages=1, mediabox="0 0 2384 3370"))

    # 页级渲染失败的样本（呈现契约见 ADR-100；人工验证 4-5 用）。
    #
    # 触发点是 **app 自己的渲染前闸门**，不是 MuPDF、也与分块渲染无关：`render_one_impl`
    # 在调用 MuPDF **之前**先算"页尺寸(pt) × 倍率"的输出像素，超过 `kMaxOutputPixels`
    # （2^28 ≈ 2.68 亿）就直接 `mark_failed(TooLarge)`。
    #   · tall  ：竖长页，**默认 fit-width 即失败** → 走"从未渲染成功"路径（画布小标 + 状态栏）；
    #   · square：方形大页，默认 fit-width 正常、**放大到约 75% 以上才失败** → 走"保留旧纹理"路径。
    #
    # 为什么不用损坏样本：truncated/random/empty 都是**打开即失败**（到不了页渲染）；改坏内容流
    # 也不行 —— MuPDF 对内容流解码错误只打印日志、照样出图（实测，见 generate_samples.py）。
    log("== 8. 页级渲染失败（超大页触发渲染前像素闸门，ADR-100 / 4-5） ==")
    write("renderfail_toobig_tall.pdf",
          minimal_pdf(1, mediabox=f"0 0 {TALL_W} {TALL_H}", contents=[tall_page_content()]))
    write("renderfail_toobig_square.pdf",
          minimal_pdf(1, mediabox=f"0 0 {SQUARE_SIDE} {SQUARE_SIDE}",
                      contents=[square_page_content()]))

    with open(os.path.join(out_dir, "_manifest.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\n样本目录：{out_dir}")


if __name__ == "__main__":
    main()
