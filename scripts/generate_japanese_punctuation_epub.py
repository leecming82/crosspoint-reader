#!/usr/bin/env python3
"""Generate a focused Japanese punctuation EPUB fixture for TTF rendering checks."""

from __future__ import annotations

import html
import uuid
import zipfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
OUTPUT = PROJECT_ROOT / "artifacts" / "japanese_punctuation_fixture.epub"
BOOK_ID = f"urn:uuid:{uuid.uuid4()}"


CSS = """\
@charset "utf-8";
html, body {
  margin: 0;
  padding: 0;
}
body {
  font-family: serif;
  line-height: 1.55;
}
h1 {
  font-size: 1.2em;
  margin: 0 0 0.8em 0;
}
p {
  margin: 0 0 0.7em 0;
}
.note {
  font-size: 0.82em;
  color: #444;
}
.sample {
  margin: 0.45em 0;
}
.grid p {
  margin-bottom: 0.35em;
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


PAGES = [
    (
        "yokogaki-closing",
        "横組み: 句読点の詰め",
        "",
        [
            "これは、句読点の位置を見るための文章です。読点、句点。読点、句点。",
            "春はあけぼの。やうやう白くなりゆく山ぎは、少し明かりて、紫だちたる雲の細くたなびきたる。",
            "吾輩は猫である。名前はまだ無い。どこで生れたか、とんと見当がつかぬ。",
            "隣の客は、よく柿食う客だ。雨、風、雲、月、そして静かな夜。",
            "比較: 日、日。日、日。あ、あ。あ、あ。",
        ],
    ),
    (
        "yokogaki-brackets",
        "横組み: 括弧と引用符",
        "",
        [
            "「これは括弧のテストです」と彼は言った。「始まり」と「終わり」の位置を見る。",
            "『日本語の組版』では、括弧、引用符、句読点の詰めが読みやすさに影響する。",
            "（丸括弧）［角括弧］｛波括弧｝〈山括弧〉《二重山括弧》を並べる。",
            "「あいうえお」、そして「かきくけこ」。『さしすせそ』、『たちつてと』。",
            "比較: 「日」「日」 『月』『月』 （山）（川） 〈風〉〈雨〉",
        ],
    ),
    (
        "tategaki-punctuation",
        "縦組み: 句読点と縦用字形",
        "vertical",
        [
            "これは縦組みの句読点を見るための文章です。読点、句点。中黒・感嘆符！疑問符？",
            "長いダッシュ――それから三点リーダー……二点リーダー‥を確認します。",
            "鉤括弧「始まり」と「終わり」、二重鉤括弧『始まり』と『終わり』。",
            "山括弧〈縦組み〉と二重山括弧《表示》、丸括弧（注）も確認します。",
            "比較: 日、月。日、月。あ、い。あ、い。",
        ],
    ),
    (
        "tategaki-mixed",
        "縦組み: 欧文・数字・ルビ",
        "vertical",
        [
            "ABCという語と2026年という数字、そして12月31日を含む縦組みです。",
            "TATECHUYOKO 12 と 34、HTTP、Wi-Fi、USB-C がどの向きで出るかを見る。",
            "吾輩<ruby>猫<rt>ねこ</rt></ruby>である。名前はまだ無い。<ruby>東京<rt>とうきょう</rt></ruby>へ行く。",
            "縦中横の候補: 1 12 123 令和8年6月11日。欧文 run: CrossPoint Reader。",
            "約物: ！ ？ ％ ＆ ／ ー ― — … ‥ ・ 、 。",
        ],
    ),
    (
        "dense-comparison",
        "比較表: 約物の連続",
        "",
        [
            "句読点: あ、い、う、え、お。か、き、く、け、こ。",
            "括弧: 「あ」『い』（う）［え］｛お｝〈か〉《き》。",
            "ダッシュ: あ―い、あ—い、あ–い、あ-い、あーーい。",
            "リーダー: あ……い、あ‥‥い、あ・・・い、あ・・・・・・い。",
            "終端: これは文末です。これは文末です、これは文末です？これは文末です！",
            "縦確認用: 「雨」『風』（雲）〈月〉《星》――……。",
        ],
    ),
]


def xhtml_page(file_id: str, title: str, cls: str, paragraphs: list[str]) -> str:
    body_class = f' class="{cls}"' if cls else ""
    p_html = "\n".join(f'    <p class="sample">{line}</p>' for line in paragraphs)
    if file_id == "dense-comparison":
      p_html = f'<div class="grid">\n{p_html}\n  </div>'
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>{html.escape(title)}</title>
  <link rel="stylesheet" type="text/css" href="../style/book.css"/>
</head>
<body{body_class}>
  <h1>{html.escape(title)}</h1>
  <p class="note">Japanese punctuation fixture: {html.escape(file_id)}</p>
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
        f'      <li><a href="xhtml/{file_id}.xhtml">{html.escape(title)}</a></li>' for file_id, title, _, _ in PAGES
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
        f'    <item id="{file_id}" href="xhtml/{file_id}.xhtml" media-type="application/xhtml+xml"/>'
        for file_id, _, _, _ in PAGES
    )
    spine_pages = "\n".join(f'    <itemref idref="{file_id}"/>' for file_id, _, _, _ in PAGES)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">{BOOK_ID}</dc:identifier>
    <dc:title>Japanese Punctuation Fixture</dc:title>
    <dc:language>ja</dc:language>
    <dc:creator>CrossPoint Test Fixtures</dc:creator>
    <meta property="dcterms:modified">2026-06-11T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="style" href="style/book.css" media-type="text/css"/>
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
        zf.writestr("OEBPS/style/book.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        for file_id, title, cls, paragraphs in PAGES:
            zf.writestr(
                f"OEBPS/xhtml/{file_id}.xhtml",
                xhtml_page(file_id, title, cls, paragraphs),
                compress_type=zipfile.ZIP_DEFLATED,
            )
    print(OUTPUT)


if __name__ == "__main__":
    write_epub()
