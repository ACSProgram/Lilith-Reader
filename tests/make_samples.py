# make_samples.py — 生成自动化测试样本集（tests/samples/，生成物不入库）
#
# 覆盖四类：
#   1. 合法文件：PDF / EPUB / FB2 / XPS / PNG / CBZ
#   2. 改名可读：图片、epub、xps、fb2 改名成别的受支持扩展名（应打开，仅提示不符）
#   3. 归档冒充：zip 图集/文档集改名 .epub/.pdf/.fb2/.cbz（应拒绝，ADR-016）
#   4. 加密与损坏：AES 加密 PDF、截断、空文件、纯文本改名、需修复的 PDF
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

def minimal_pdf(pages=1, mediabox="0 0 200 300", mediaboxes=None):
    """结构合法、xref 正确的多页最小 PDF。

    mediaboxes 给出时按页指定 MediaBox（用于构造**异构页尺寸**样本）；
    否则所有页用同一个 mediabox。

    对象编号约定（务必与下面的 append 顺序一致）：
      1 Catalog / 2 Pages / 3 Resources / 4 Font
      之后每页占两个对象：Page = 5+2i，Contents 流 = 6+2i
    """
    if mediaboxes is not None:
        assert len(mediaboxes) == pages
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
        stream = b"BT /F1 24 Tf 20 150 Td (page) Tj ET"
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
    write("comic.cbz", zip_of([("1.png", png), ("2.png", png_1x1(120)), ("3.png", png_1x1(60))]))
    # 带两级目录的 PDF：供 doc_test 断言 outline() 的层级/页号解析
    write("outline.pdf", pdf_with_outline(3))

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

    log("== 5. 需修复但应能打开（容错用例） ==")
    write("junk_before_header.pdf", b"JUNK" * 64 + pdf3)
    write("no_startxref.pdf", pdf3.replace(b"startxref", b"startXref"))

    log("== 6. 加密 PDF（需要 PyMuPDF） ==")
    make_encrypted(out_dir, log)

    with open(os.path.join(out_dir, "_manifest.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\n样本目录：{out_dir}")


if __name__ == "__main__":
    main()
