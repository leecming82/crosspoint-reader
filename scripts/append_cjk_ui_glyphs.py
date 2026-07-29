#!/usr/bin/env python3
"""Append individual glyphs to an existing generated CJK UI font header.

generate_cjk_ui_font.py rebuilds a header from scratch, which rewrites every
glyph in it. When only a handful of codepoints are missing that is a poor trade:
a different Pillow/FreeType build shifts rasterization across the whole table,
so thousands of unrelated glyphs change to add a few. This script instead
rasterizes only the requested codepoints and splices them into the existing
arrays, leaving every other glyph byte-identical.

Rasterization mirrors generate_cjk_ui_font.py exactly (same cell fitting, same
fixed baseline, same 1-bit packing, same full-width advance for non-ASCII) so
appended glyphs are indistinguishable from generated ones.

Usage:
    python3 scripts/append_cjk_ui_glyphs.py \\
        --header lib/GfxRenderer/cjk_ui_font_17.h \\
        --font test/NotoSerifJP-VariableFont_wght.ttf \\
        --variation-name Medium \\
        --codepoints FE45 FE46 25CF 25CB 25E6 25B2 25B3 25C9 25CE
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Error: PIL/Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)


def load_font_fitting_cell(font_path, pixel_size, variation_name=None):
    """Identical to generate_cjk_ui_font.py: shrink until the ascent fits the cell."""
    pt_size = max(1, int(pixel_size))
    while pt_size > 0:
        try:
            font = ImageFont.truetype(font_path, pt_size)
            if variation_name:
                font.set_variation_by_name(variation_name)
        except Exception as e:
            print(f"Error loading font: {e}")
            return None, None, None, None
        ascent, descent = font.getmetrics()
        if ascent <= pixel_size:
            return font, pt_size, ascent, descent
        pt_size -= 1
    return None, None, None, None


def bitmap_bytes_from_image(img, pixel_size):
    bytes_per_row = (pixel_size + 7) // 8
    out = []
    for row in range(pixel_size):
        for byte_idx in range(bytes_per_row):
            byte_val = 0
            for bit in range(8):
                px = byte_idx * 8 + bit
                if px < pixel_size and img.getpixel((px, row)):
                    byte_val |= 1 << (7 - bit)
            out.append(byte_val)
    return out


def rasterize(font, cp, pixel_size, baseline):
    """Draw one codepoint into the cell the way the generator does."""
    char = chr(cp)
    img = Image.new("1", (pixel_size, pixel_size), 0)
    draw = ImageDraw.Draw(img)

    try:
        bbox = font.getbbox(char)
        char_width = (bbox[2] - bbox[0]) if bbox else pixel_size // 2
    except Exception:
        char_width = pixel_size // 2

    x = 0 if char_width > pixel_size - 2 else 1
    draw.text((x, baseline), char, font=font, fill=1, anchor="ls")

    # Non-ASCII glyphs are full-width; ASCII would use char_width + 2 (see generator).
    width = pixel_size if cp >= 0x80 else min(char_width + 2, pixel_size)
    return bitmap_bytes_from_image(img, pixel_size), width


def parse_array(text, name, converter):
    m = re.search(rf"{name}\[\] PROGMEM = \{{(.*?)\}};", text, re.S)
    if not m:
        raise SystemExit(f"Could not find array {name} in header")
    body = m.group(1)
    # Strip the "// U+XXXX (c)" per-glyph comments in the bitmap blob.
    body = re.sub(r"//[^\n]*", "", body)
    return [converter(tok) for tok in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)], m


def parse_param(text, name):
    m = re.search(rf"{name} = (\d+);", text)
    if not m:
        raise SystemExit(f"Could not find parameter {name}")
    return int(m.group(1))


def wrap_rows(tokens, per_row=16):
    rows = []
    for i in range(0, len(tokens), per_row):
        rows.append("    " + " ".join(tokens[i : i + per_row]))
    return "\n".join(rows)


def render_header(path, codepoints, widths, bitmaps, bytes_per_char, original):
    """Splice updated arrays into the original text, leaving everything else alone.

    Only the counts in the banner comment, CJK_UI_FONT_GLYPH_COUNT, and the three
    array bodies change; the preamble, includes, namespace and accessor functions
    are preserved verbatim. Emitted in the generator's raw layout -- run
    clang-format afterwards, since CI reformats this path (it is not excluded by
    bin/clang-format-fix).
    """
    total = len(codepoints) * bytes_per_char
    text = original

    text = re.sub(r"\* Characters: \d+", f"* Characters: {len(codepoints)}", text, count=1)
    text = re.sub(
        r"\* Total size: \d+ bytes \([\d.]+ KB\)",
        f"* Total size: {total} bytes ({total / 1024:.1f} KB)",
        text,
        count=1,
    )
    text = re.sub(
        r"(CJK_UI_FONT_GLYPH_COUNT = )\d+;",
        rf"\g<1>{len(codepoints)};",
        text,
        count=1,
    )

    def replace_array(src, name, body):
        pattern = rf"({name}\[\] PROGMEM = \{{)(.*?)(\}};)"
        return re.sub(pattern, lambda m: m.group(1) + "\n" + body + "\n" + m.group(3), src, count=1, flags=re.S)

    text = replace_array(text, "CJK_UI_CODEPOINTS", wrap_rows([f"0x{cp:04X}," for cp in codepoints]))
    text = replace_array(text, "CJK_UI_GLYPH_WIDTHS", wrap_rows([f"{w}," for w in widths]))

    glyph_rows = []
    for cp, bitmap in zip(codepoints, bitmaps):
        ch = chr(cp)
        label = ch if ch.isprintable() else f"U+{cp:04X}"
        glyph_rows.append(f"    // U+{cp:04X} ({label})")
        glyph_rows.append(wrap_rows([f"0x{b:02X}," for b in bitmap]))
    text = replace_array(text, "CJK_UI_GLYPHS", "\n".join(glyph_rows))

    path.write_text(text, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True, type=Path)
    ap.add_argument("--font", required=True)
    ap.add_argument("--variation-name", default=None)
    ap.add_argument("--codepoints", nargs="+", required=True, help="Hex codepoints, e.g. FE45 25CF")
    args = ap.parse_args()

    original = args.header.read_text(encoding="utf-8")
    pixel_size = parse_param(original, "CJK_UI_FONT_WIDTH")
    bytes_per_row = parse_param(original, "CJK_UI_FONT_BYTES_PER_ROW")
    bytes_per_char = parse_param(original, "CJK_UI_FONT_BYTES_PER_CHAR")
    glyph_count = parse_param(original, "CJK_UI_FONT_GLYPH_COUNT")

    codepoints, _ = parse_array(original, "CJK_UI_CODEPOINTS", lambda t: int(t, 16))
    widths, _ = parse_array(original, "CJK_UI_GLYPH_WIDTHS", lambda t: int(t, 0))
    flat, _ = parse_array(original, "CJK_UI_GLYPHS", lambda t: int(t, 16))

    if len(codepoints) != glyph_count or len(widths) != glyph_count:
        raise SystemExit(f"Header inconsistent: {len(codepoints)} codepoints, {len(widths)} widths, count={glyph_count}")
    if len(flat) != glyph_count * bytes_per_char:
        raise SystemExit(f"Bitmap blob is {len(flat)} bytes, expected {glyph_count * bytes_per_char}")

    bitmaps = [flat[i * bytes_per_char : (i + 1) * bytes_per_char] for i in range(glyph_count)]

    font, pt_size, ascent, descent = load_font_fitting_cell(args.font, pixel_size, args.variation_name)
    if font is None:
        raise SystemExit("Could not load font")
    baseline = pixel_size - descent
    print(f"Cell {pixel_size}px, pt={pt_size}, ascent={ascent}, descent={descent}, baseline={baseline}")

    added = 0
    for token in args.codepoints:
        cp = int(token, 16)
        if cp in codepoints:
            print(f"  U+{cp:04X} already present, skipping")
            continue
        if cp > 0xFFFF:
            raise SystemExit(f"U+{cp:04X} exceeds the uint16_t codepoint table")
        bitmap, width = rasterize(font, cp, pixel_size, baseline)
        if not any(bitmap):
            print(f"  U+{cp:04X} rasterized blank -- font lacks the glyph; refusing to add")
            continue
        idx = 0
        while idx < len(codepoints) and codepoints[idx] < cp:
            idx += 1
        codepoints.insert(idx, cp)
        widths.insert(idx, width)
        bitmaps.insert(idx, bitmap)
        ink = sum(bin(b).count("1") for b in bitmap)
        print(f"  U+{cp:04X} {chr(cp)} added at index {idx} (width {width}, {ink} px ink)")
        added += 1

    if added == 0:
        print("Nothing to do.")
        return

    render_header(args.header, codepoints, widths, bitmaps, bytes_per_char, original)
    print(f"Wrote {args.header}: {glyph_count} -> {len(codepoints)} glyphs")
    print("Now run clang-format on the header (CI reformats this path).")


if __name__ == "__main__":
    main()
