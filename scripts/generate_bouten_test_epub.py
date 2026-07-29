#!/usr/bin/env python3
"""Generate a 傍点 (bouten / emphasis mark) EPUB fixture.

Covers the two ways real Japanese EPUBs express emphasis marks:

  1. Ruby route  -- <ruby>重<rt>・</rt></ruby>, one mark character per base glyph.
     Renders through the normal ruby path, so it works whenever the ruby font
     covers the mark codepoint.
  2. CSS route   -- <span class="em-sesame">, with the mark implied by
     -epub-text-emphasis-style. No mark character exists in the document.

Each mark line prints the raw mark character in the body text as well, so a
line where the label renders but the ruby does not isolates the miss to the
ruby font rather than to the body font.

The CSS is split across a directly-linked sheet and an @import-only sheet
(mirroring the EBPAJ layout used by commercial Japanese books) so the fixture
also shows whether imported rules reach the renderer.
"""

from __future__ import annotations

import html
import uuid
import zipfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
OUTPUT = PROJECT_ROOT / "artifacts" / "bouten_fixture.epub"
BOOK_ID = f"urn:uuid:{uuid.uuid4()}"

# Base word every sample marks up: 3 glyphs, so each line shows 3 marks.
BASE = "重要語"

# Candidate mark codepoints. The first three are present in the built-in 17px
# CJK set used for ruby; the rest are not, and are expected to drop out.
MARKS = [
    ("U+30FB", "・", "katakana middle dot"),
    ("U+2022", "•", "bullet"),
    ("U+00B7", "·", "middle dot"),
    ("U+FE45", "﹅", "sesame dot"),
    ("U+FE46", "﹆", "white sesame dot"),
    ("U+25CF", "●", "black circle"),
    ("U+25CB", "○", "white circle"),
    ("U+25E6", "◦", "white bullet"),
    ("U+25B2", "▲", "black triangle"),
    ("U+25B3", "△", "white triangle"),
    ("U+25C9", "◉", "fisheye"),
    ("U+25CE", "◎", "bullseye"),
]

# EBPAJ class names, exactly as commercial Japanese EPUBs declare them.
CSS_VARIANTS = [
    ("em-sesame", "filled sesame"),
    ("em-sesame-open", "open sesame"),
    ("em-dot", "filled dot"),
    ("em-dot-open", "open dot"),
    ("em-circle", "filled circle"),
    ("em-circle-open", "open circle"),
    ("em-double-circle", "filled double-circle"),
    ("em-double-circle-open", "open double-circle"),
    ("em-triangle", "filled triangle"),
    ("em-triangle-open", "open triangle"),
]


BOOK_CSS = """\
@charset "utf-8";
@import "emphasis.css";

html, body {
  margin: 0;
  padding: 0;
}
body {
  font-family: serif;
  line-height: 1.6;
}
h1 {
  font-size: 1.15em;
  margin: 0 0 0.7em 0;
}
p {
  margin: 0 0 0.6em 0;
}
.note {
  font-size: 0.8em;
}
.label {
  font-size: 0.8em;
}
.vertical {
  writing-mode: vertical-rl;
  -epub-writing-mode: vertical-rl;
  -webkit-writing-mode: vertical-rl;
}
rt {
  font-size: 0.5em;
}
"""

# Reached ONLY through the @import above. Rules here are declared in the OPF
# manifest like every other sheet, so this doubles as a probe: if .import-probe
# centres, imported sheets are reaching the parser.
EMPHASIS_CSS = """\
@charset "utf-8";

/* 圏点・傍点 -- EBPAJ standard declarations */
.em-sesame {
  -webkit-text-emphasis-style: filled sesame;
  -epub-text-emphasis-style:   filled sesame;
}
.em-sesame-open {
  -webkit-text-emphasis-style: open sesame;
  -epub-text-emphasis-style:   open sesame;
}
.em-dot {
  -webkit-text-emphasis-style: filled dot;
  -epub-text-emphasis-style:   filled dot;
}
.em-dot-open {
  -webkit-text-emphasis-style: open dot;
  -epub-text-emphasis-style:   open dot;
}
.em-circle {
  -webkit-text-emphasis-style: filled circle;
  -epub-text-emphasis-style:   filled circle;
}
.em-circle-open {
  -webkit-text-emphasis-style: open circle;
  -epub-text-emphasis-style:   open circle;
}
.em-double-circle {
  -webkit-text-emphasis-style: filled double-circle;
  -epub-text-emphasis-style:   filled double-circle;
}
.em-double-circle-open {
  -webkit-text-emphasis-style: open double-circle;
  -epub-text-emphasis-style:   open double-circle;
}
.em-triangle {
  -webkit-text-emphasis-style: filled triangle;
  -epub-text-emphasis-style:   filled triangle;
}
.em-triangle-open {
  -webkit-text-emphasis-style: open triangle;
  -epub-text-emphasis-style:   open triangle;
}

/* Diagnostic: a property the renderer DOES support, declared only in this
   imported sheet. If the sample centres, imported rules are being applied. */
.import-probe {
  text-align: center;
}
"""


def ruby_marked(base: str, mark: str) -> str:
    """One <ruby> group per base glyph, each annotated with the mark."""
    return "".join(f"<ruby>{ch}<rt>{mark}</rt></ruby>" for ch in base)


def ruby_lines() -> list[str]:
    lines = [
        '<span class="label">furigana control:</span> '
        + "<ruby>漢<rt>かん</rt></ruby><ruby>字<rt>じ</rt></ruby>"
    ]
    for code, mark, name in MARKS:
        lines.append(
            f'<span class="label">{code} {html.escape(mark)} {html.escape(name)}:</span> '
            + ruby_marked(BASE, html.escape(mark))
        )
    return lines


def css_lines() -> list[str]:
    lines = []
    for cls, desc in CSS_VARIANTS:
        lines.append(
            f'<span class="label">{html.escape(desc)}:</span> '
            f'この<span class="{cls}">{BASE}</span>に傍点。'
        )
    return lines


REALISTIC = [
    "　また、<span class=\"em-sesame\">あれ</span>が訪ねてきた。",
    "　<ruby>彼女<rt>かのじょ</rt></ruby>は流しの前に立ったままで、"
    "勝手口の方に目をやった。すると表で<ruby>佇<rt>たたず</rt></ruby>んでいる"
    "<span class=\"em-sesame\">それ</span>も、戸口越しにこちらを凝視している、"
    "そんな気がしてきた。",
    "　つまり何らかの事情で生活に困り、職を探すが何処も雇ってくれず、"
    "仕方なく警備員<span class=\"em-sesame\">でも</span>するかと応募したものの、"
    "実は警備員<span class=\"em-sesame\">しか</span>できないような、"
    "そんな人物ばかりが目についたのだ。",
]

PROBE = [
    '<span class="label">import probe (should be centred if imported CSS applies):</span>',
    '<span class="import-probe">この行は emphasis.css の text-align: center で中央寄せされるはず。</span>',
]


PAGES = [
    ("ruby-tategaki", "縦組み: ルビ方式の傍点", "vertical",
     "Ruby route, vertical. Marks that render come from the ruby font.", ruby_lines()),
    ("ruby-yokogaki", "横組み: ルビ方式の傍点", "",
     "Ruby route, horizontal. Same marks as the previous page.", ruby_lines()),
    ("css-tategaki", "縦組み: CSS方式の傍点", "vertical",
     "CSS route, vertical. EBPAJ class names, marks implied by text-emphasis-style.", css_lines()),
    ("css-yokogaki", "横組み: CSS方式の傍点", "",
     "CSS route, horizontal.", css_lines()),
    ("realistic", "縦組み: 実書籍相当", "vertical",
     "Passages copied in shape from the two Kadokawa test books.", REALISTIC),
    ("import-probe", "CSS @import 到達確認", "",
     "Checks whether rules from an @import-only stylesheet are applied.", PROBE),
]


def xhtml_page(file_id: str, title: str, cls: str, note: str, paragraphs: list[str]) -> str:
    body_class = f' class="{cls}"' if cls else ""
    p_html = "\n".join(f"    <p>{line}</p>" for line in paragraphs)
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>{html.escape(title)}</title>
  <link rel="stylesheet" type="text/css" href="../style/book.css"/>
</head>
<body{body_class}>
  <h1>{html.escape(title)}</h1>
  <p class="note">{html.escape(note)}</p>
{p_html}
</body>
</html>
"""


def container_xml() -> str:
    return """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""


def nav_xhtml() -> str:
    links = "\n".join(
        f'      <li><a href="xhtml/{fid}.xhtml">{html.escape(title)}</a></li>'
        for fid, title, _, _, _ in PAGES
    )
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="ja" xml:lang="ja">
<head>
  <title>目次</title>
  <link rel="stylesheet" type="text/css" href="style/book.css"/>
</head>
<body>
  <nav epub:type="toc" id="toc">
    <h1>目次</h1>
    <ol>
{links}
    </ol>
  </nav>
</body>
</html>
"""


def content_opf() -> str:
    manifest_pages = "\n".join(
        f'    <item id="{fid}" href="xhtml/{fid}.xhtml" media-type="application/xhtml+xml"/>'
        for fid, _, _, _, _ in PAGES
    )
    spine_pages = "\n".join(f'    <itemref idref="{fid}"/>' for fid, _, _, _, _ in PAGES)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">{BOOK_ID}</dc:identifier>
    <dc:title>傍点テスト Bouten Fixture</dc:title>
    <dc:language>ja</dc:language>
    <dc:creator>CrossPoint Test Fixtures</dc:creator>
    <meta property="dcterms:modified">2026-07-29T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="style" href="style/book.css" media-type="text/css"/>
    <item id="style-emphasis" href="style/emphasis.css" media-type="text/css"/>
{manifest_pages}
  </manifest>
  <spine>
{spine_pages}
  </spine>
</package>
"""


def write_epub() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUTPUT, "w") as zf:
        zf.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", container_xml(), compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/content.opf", content_opf(), compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/nav.xhtml", nav_xhtml(), compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/style/book.css", BOOK_CSS, compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr("OEBPS/style/emphasis.css", EMPHASIS_CSS, compress_type=zipfile.ZIP_DEFLATED)
        for fid, title, cls, note, paragraphs in PAGES:
            zf.writestr(
                f"OEBPS/xhtml/{fid}.xhtml",
                xhtml_page(fid, title, cls, note, paragraphs),
                compress_type=zipfile.ZIP_DEFLATED,
            )
    print(OUTPUT)


if __name__ == "__main__":
    write_epub()
