#!/usr/bin/env python3
"""fb2 / fb2.zip -> EPUB со своим оформлением для Digma Luma (движок MuPDF).

Читалка даёт только фиксированные шаги (интервал 1.0/1.5/2.0, 6 вариантов полей).
CSS книги MuPDF применяет поверх, поэтому тонкая настройка зашивается прямо в EPUB:

  python3 fb2epub.py книга.fb2 [-o out.epub] [--font fonts/PTSerif] [--size 1.0]
      [--line-height 1.3] [--indent 1.5] [--margin 0.5] [--para-gap 0]
      [--align justify|left] [--no-hyphen]

--font      префикс шрифта: ищутся <префикс>-Regular/-Italic/-Bold/-BoldItalic .ttf/.otf
--size      множитель размера относительно выбранного в читалке
--line-height  межстрочный интервал (1.0 … 2.0, любой шаг)
--indent    красная строка, em
--margin    дополнительные поля слева/справа, em (к полям самой читалки)
--para-gap  отступ между абзацами, em
Мягкие переносы (U+00AD) расставляются по словарю pyphen, если он установлен.
"""
import argparse
import base64
import html
import os
import re
import sys
import uuid
import zipfile
import xml.etree.ElementTree as ET

FB = "{http://www.gribuser.ru/xml/fictionbook/2.0}"
XLINK = "{http://www.w3.org/1999/xlink}href"
WORD = re.compile(r"[А-Яа-яЁёA-Za-z]{5,}")


def load_fb2(path):
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            name = next(n for n in z.namelist() if n.lower().endswith(".fb2"))
            data = z.read(name)
    else:
        data = open(path, "rb").read()
    return ET.fromstring(data)


class Converter:
    def __init__(self, hyph):
        self.hyph = hyph
        self.images = {}       # id -> (имя файла, mime)
        self.note_ids = set()

    def text(self, s):
        if not s:
            return ""
        s = html.escape(s, quote=False)
        if self.hyph:
            s = WORD.sub(lambda m: self.hyph.inserted(m.group(0), hyphen="­"), s)
        return s

    def inline(self, el):
        out = [self.text(el.text)]
        for ch in el:
            tag = ch.tag.replace(FB, "")
            inner = self.inline(ch)
            if tag == "emphasis":
                out.append(f"<em>{inner}</em>")
            elif tag == "strong":
                out.append(f"<strong>{inner}</strong>")
            elif tag in ("sup", "sub", "code"):
                out.append(f"<{tag}>{inner}</{tag}>")
            elif tag == "strikethrough":
                out.append(f"<s>{inner}</s>")
            elif tag == "a":
                href = ch.get(XLINK, "")
                if href.startswith("#"):
                    href = ("notes.xhtml" if href[1:] in self.note_ids else "") + href
                cls = ' class="note"' if ch.get("type") == "note" else ""
                out.append(f'<a href="{html.escape(href)}"{cls}>{inner}</a>')
            elif tag == "image":
                out.append(self.image(ch, inline=True))
            else:
                out.append(inner)
            out.append(self.text(ch.tail))
        return "".join(out)

    def image(self, el, inline=False):
        ref = el.get(XLINK, "").lstrip("#")
        if ref not in self.images:
            return ""
        src = "images/" + self.images[ref][0]
        return f'<img src="{src}" alt=""/>' if inline else f'<div class="img"><img src="{src}" alt=""/></div>'

    def block(self, el, depth):
        out = []
        for ch in el:
            tag = ch.tag.replace(FB, "")
            idattr = f' id="{html.escape(ch.get("id"))}"' if ch.get("id") else ""
            if tag == "p":
                out.append(f"<p{idattr}>{self.inline(ch)}</p>")
            elif tag == "title":
                h = min(depth + 1, 6)
                lines = [self.inline(p) for p in ch if p.tag == FB + "p"]
                out.append(f"<h{h}{idattr}>{'<br/>'.join(lines)}</h{h}>")
            elif tag == "subtitle":
                out.append(f'<p class="subtitle"{idattr}>{self.inline(ch)}</p>')
            elif tag == "empty-line":
                out.append('<p class="empty"> </p>')
            elif tag == "image":
                out.append(self.image(ch))
            elif tag == "section":
                out.append(f"<div class=\"section\"{idattr}>{self.block(ch, depth + 1)}</div>")
            elif tag in ("epigraph", "cite", "poem", "stanza", "annotation"):
                out.append(f'<div class="{tag}"{idattr}>{self.block(ch, depth)}</div>')
            elif tag == "v":
                out.append(f'<p class="v">{self.inline(ch)}</p>')
            elif tag == "text-author":
                out.append(f'<p class="author">{self.inline(ch)}</p>')
            elif tag == "date":
                out.append(f'<p class="date">{self.inline(ch)}</p>')
            elif tag == "table":
                rows = []
                for tr in ch:
                    cells = "".join(f"<{c.tag.replace(FB, '')}>{self.inline(c)}</{c.tag.replace(FB, '')}>" for c in tr)
                    rows.append(f"<tr>{cells}</tr>")
                out.append(f"<table>{''.join(rows)}</table>")
        return "\n".join(out)


def css(a, has_font):
    font = "font-family: 'Book', serif;" if has_font else ""
    faces = ""
    if has_font:
        for fn, w, st in has_font:
            faces += f"@font-face {{ font-family: 'Book'; src: url(fonts/{fn}); font-weight: {w}; font-style: {st}; }}\n"
    align = "justify" if a.align == "justify" else "left"
    return f"""{faces}
body {{ margin: 0 {a.margin}em; padding: 0; }}
p, .v, li {{ {font} font-size: {a.size}em; line-height: {a.line_height}; }}
p {{ margin: {a.para_gap}em 0 0 0; text-indent: {a.indent}em; text-align: {align}; }}
h1, h2, h3, h4, h5, h6 {{ {font} text-align: center; text-indent: 0; line-height: 1.2; margin: 1.2em 0 0.8em; page-break-after: avoid; }}
h1 {{ font-size: {1.6 * a.size:.2f}em; }} h2 {{ font-size: {1.35 * a.size:.2f}em; }} h3, h4, h5, h6 {{ font-size: {1.15 * a.size:.2f}em; }}
.section > h2, body > h1 {{ page-break-before: always; }}
.subtitle {{ text-align: center; text-indent: 0; font-weight: bold; margin: 1em 0; }}
.empty {{ margin: 0; text-indent: 0; }}
.epigraph {{ margin: 1em 0 1em 30%; font-style: italic; }}
.epigraph p, .cite p, .stanza p {{ text-indent: 0; text-align: left; }}
.cite {{ margin: 1em 1.5em; }}
.poem {{ margin: 1em 0 1em 2em; }}
.stanza {{ margin: 0 0 0.8em 0; }}
.v {{ text-indent: 0; margin: 0; text-align: left; }}
.author {{ text-align: right; text-indent: 0; font-style: italic; }}
.date {{ text-align: right; text-indent: 0; }}
.img {{ text-align: center; margin: 0.5em 0; text-indent: 0; }}
img {{ max-width: 100%; }}
a.note {{ font-size: 0.7em; vertical-align: super; line-height: 1; text-decoration: none; }}
"""


def xhtml(title, body):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ru"><head><title>{html.escape(title)}</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head><body>
{body}
</body></html>"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("-o", "--out")
    ap.add_argument("--font", help="префикс файлов шрифта, напр. fonts/PTSerif")
    ap.add_argument("--size", type=float, default=1.0)
    ap.add_argument("--line-height", type=float, default=1.3)
    ap.add_argument("--indent", type=float, default=1.5)
    ap.add_argument("--margin", type=float, default=0.0)
    ap.add_argument("--para-gap", type=float, default=0.0)
    ap.add_argument("--align", choices=("justify", "left"), default="justify")
    ap.add_argument("--no-hyphen", action="store_true")
    a = ap.parse_args()

    hyph = None
    if not a.no_hyphen:
        try:
            import pyphen
            hyph = pyphen.Pyphen(lang="ru_RU", left=2, right=2)
        except ImportError:
            print("pyphen не установлен — без переносов (pip3 install pyphen)", file=sys.stderr)

    root = load_fb2(a.src)
    ti = root.find(f"{FB}description/{FB}title-info")
    title = (ti.findtext(f"{FB}book-title") or os.path.basename(a.src)).strip()
    authors = []
    for au in ti.findall(f"{FB}author"):
        parts = [au.findtext(FB + k) for k in ("first-name", "middle-name", "last-name")]
        authors.append(" ".join(p.strip() for p in parts if p and p.strip()))
    author = ", ".join(x for x in authors if x)

    conv = Converter(hyph)
    bins = {}
    for i, b in enumerate(root.findall(f"{FB}binary")):
        mime = b.get("content-type", "image/jpeg")
        ext = {"image/png": "png", "image/gif": "gif"}.get(mime, "jpg")
        name = f"img{i}.{ext}"
        conv.images[b.get("id")] = (name, mime)
        bins[name] = base64.b64decode(b.text or "")

    bodies = root.findall(f"{FB}body")
    main_body = bodies[0]
    notes = [b for b in bodies[1:]]
    for nb in notes:
        for el in nb.iter():
            if el.get("id"):
                conv.note_ids.add(el.get("id"))

    # главы: верхние секции основного тела, каждая в свой файл
    chapters = []
    head = []
    for ch in main_body:
        tag = ch.tag.replace(FB, "")
        if tag == "section":
            chapters.append(xhtml(title, f'<div class="section">{conv.block(ch, 1)}</div>'))
        else:
            wrap = ET.Element("x")
            wrap.append(ch)
            head.append(conv.block(wrap, 0))
    if head:
        chapters.insert(0, xhtml(title, "\n".join(head)))
    notes_html = None
    if notes:
        notes_html = xhtml("Примечания", "\n".join(conv.block(nb, 1) for nb in notes))

    cover = ti.find(f"{FB}coverpage/{FB}image")
    cover_name = conv.images.get((cover.get(XLINK) or "").lstrip("#"), (None,))[0] if cover is not None else None

    fonts = []
    if a.font:
        for style, w, st in (("Regular", "normal", "normal"), ("Italic", "normal", "italic"),
                             ("Bold", "bold", "normal"), ("BoldItalic", "bold", "italic")):
            for ext in ("ttf", "otf"):
                p = f"{a.font}-{style}.{ext}"
                if os.path.exists(p):
                    fonts.append((os.path.basename(p), w, st, p))
                    break
        if not fonts:
            sys.exit(f"шрифт {a.font}-Regular.ttf не найден")

    out = a.out or re.sub(r"(\.fb2)?(\.zip)?$", "", a.src, flags=re.I) + ".epub"
    uid = str(uuid.uuid4())
    manifest, spine = [], []
    with zipfile.ZipFile(out, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zc = lambda n, d: z.writestr(n, d, compress_type=zipfile.ZIP_DEFLATED)
        zc("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        zc("OEBPS/style.css", css(a, [f[:3] for f in fonts]))
        manifest.append('<item id="css" href="style.css" media-type="text/css"/>')
        for i, c in enumerate(chapters):
            zc(f"OEBPS/ch{i:03}.xhtml", c)
            manifest.append(f'<item id="ch{i}" href="ch{i:03}.xhtml" media-type="application/xhtml+xml"/>')
            spine.append(f'<itemref idref="ch{i}"/>')
        if notes_html:
            zc("OEBPS/notes.xhtml", notes_html)
            manifest.append('<item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>')
            spine.append('<itemref idref="notes"/>')
        for name, data in bins.items():
            zc(f"OEBPS/images/{name}", data)
            mime = dict(conv.images.values())[name]
            props = ' properties="cover-image"' if name == cover_name else ""
            manifest.append(f'<item id="{name}" href="images/{name}" media-type="{mime}"{props}/>')
        for fn, _, _, p in fonts:
            zc(f"OEBPS/fonts/{fn}", open(p, "rb").read())
            manifest.append(f'<item id="{fn}" href="fonts/{fn}" media-type="font/ttf"/>')
        zc("OEBPS/nav.xhtml", xhtml("Содержание", f'<nav epub:type="toc" xmlns:epub="http://www.idpf.org/2007/ops"><ol>'
                                    + "".join(f'<li><a href="ch{i:03}.xhtml">{i + 1}</a></li>' for i in range(len(chapters)))
                                    + "</ol></nav>"))
        manifest.append('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>')
        cover_meta = f'<meta name="cover" content="{cover_name}"/>' if cover_name else ""
        zc("OEBPS/content.opf", f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="uid">urn:uuid:{uid}</dc:identifier><dc:title>{html.escape(title)}</dc:title>
<dc:creator>{html.escape(author)}</dc:creator><dc:language>ru</dc:language>{cover_meta}
</metadata><manifest>{''.join(manifest)}</manifest><spine>{''.join(spine)}</spine></package>""")
    print(f"{out}: {len(chapters)} глав, {len(bins)} картинок, шрифт: {fonts[0][0] if fonts else 'из читалки'}")


if __name__ == "__main__":
    main()
