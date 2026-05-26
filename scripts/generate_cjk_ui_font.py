#!/usr/bin/env python3
"""
Generate CJK UI font header file for CrossPoint Reader.
Uses a supplied CJK-capable TTF/OTF font to generate bitmap font data.

Usage:
    python3 generate_cjk_ui_font.py --size 20 --font /path/to/NotoSansJP-VariableFont_wght.ttf --variation-name Medium
"""

import argparse
import glob
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("Error: PyYAML not installed. Run: pip3 install PyYAML")
    sys.exit(1)

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Error: PIL/Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

# Base UI characters: ASCII, punctuation, kana
# CJK characters are auto-extracted from translation YAML files at build time
BASE_UI_CHARS = """
!%&()*+,-./0123456789:;<=>?@，。！、：；？""''「」『』【】〈〉《》〔〕…‥—―‐‑–－︙︰︱︲﹣─·•（）«»
・／□＆
ABCDEFGHIJKLMNOPQRSTUVWXYZ[]{}_
abcdefghijklmnopqrstuvwxyz|
０１２３４５６７８９
ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ
ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ
あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん
がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ
ぁぃぅぇぉっゃゅょゔ
アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン
ガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ
ァィゥェォッャュョヴヵヶー゠
一二三四五六七八九十百千万亿
黎
"""

CJK_UI_VERTICAL_PROLONGED_SOUND_MARK = 0xE000
CJK_UI_VERTICAL_DOUBLE_HYPHEN = 0xE001


def extract_chars_from_translations(translations_dir):
    """Extract all unique non-ASCII characters from translation YAML files."""
    chars = set()
    for path in sorted(glob.glob(str(Path(translations_dir) / "*.yaml"))):
        with open(path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
        for key, value in data.items():
            if key.startswith("_"):
                continue
            for c in str(value):
                if ord(c) >= 0x80:
                    chars.add(c)
    return chars


# Extract unique characters
def get_unique_chars(base_text, translations_dir=None, codepoints_file=None):
    chars = set()
    for c in base_text:
        if c.strip() and ord(c) >= 0x20:
            chars.add(c)
    if translations_dir:
        i18n_chars = extract_chars_from_translations(translations_dir)
        chars.update(i18n_chars)
        print(f"  Extracted {len(i18n_chars)} characters from translations")
    if codepoints_file:
        cp_chars = set()
        with open(codepoints_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    cp = int(line, 16)
                    if cp >= 0x20:
                        cp_chars.add(chr(cp))
                except ValueError:
                    pass
        chars.update(cp_chars)
        print(f"  Added {len(cp_chars)} characters from codepoints file")
    return sorted(chars, key=ord)

def load_font_fitting_cell(font_path, pixel_size, variation_name=None):
    """Load a font and shrink it until ascent fits the cell height.

    Only ascent is required to fit; descent may be clipped at the bottom.
    CJK glyphs rarely use descender space, so this keeps the visual size
    consistent across font weights (e.g. Medium vs Bold).
    """
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

def find_vertical_alternate_glyph_id(font_path, source_cp):
    """Return a glyph ID for source_cp's vert/vrt2 alternate, if any."""
    from fontTools.ttLib import TTFont

    font = TTFont(font_path)
    try:
        cmap = font.getBestCmap() or {}
        source_glyph = cmap.get(source_cp)
        if not source_glyph or "GSUB" not in font:
            return None

        gsub = font["GSUB"].table
        lookup_indices = set()
        if gsub.FeatureList:
            for feature_record in gsub.FeatureList.FeatureRecord:
                if feature_record.FeatureTag in ("vert", "vrt2"):
                    lookup_indices.update(feature_record.Feature.LookupListIndex)

        for lookup_index in lookup_indices:
            lookup = gsub.LookupList.Lookup[lookup_index]
            for subtable in lookup.SubTable:
                actual = subtable.ExtSubTable if lookup.LookupType == 7 and hasattr(subtable, "ExtSubTable") else subtable
                mapping = getattr(actual, "mapping", None)
                if mapping and source_glyph in mapping:
                    return font.getGlyphID(mapping[source_glyph])
    finally:
        font.close()
    return None

def rasterize_glyph_id(font_path, glyph_id, pixel_size, pt_size, baseline, variation_name=None):
    """Rasterize a glyph ID into the same 1-bit cell format as normal UI glyphs."""
    import freetype

    face = freetype.Face(font_path)
    try:
        face.set_pixel_sizes(0, pt_size)
        if variation_name:
            try:
                face.set_var_named_instance(variation_name)
            except Exception:
                pass
        face.load_glyph(glyph_id, freetype.FT_LOAD_RENDER)
        bitmap = face.glyph.bitmap
        char_width = bitmap.width if bitmap.width > 0 else pixel_size // 2
        x = 0 if char_width > pixel_size - 2 else 1
        left = x + face.glyph.bitmap_left
        top = baseline - face.glyph.bitmap_top

        img = Image.new("1", (pixel_size, pixel_size), 0)
        buf = bitmap.buffer
        abs_pitch = abs(bitmap.pitch)
        for row in range(bitmap.rows):
            row_offset = row * abs_pitch if bitmap.pitch >= 0 else (bitmap.rows - 1 - row) * abs_pitch
            for col in range(bitmap.width):
                dst_x = left + col
                dst_y = top + row
                if 0 <= dst_x < pixel_size and 0 <= dst_y < pixel_size and buf[row_offset + col] > 0:
                    img.putpixel((dst_x, dst_y), 1)

        return img, pixel_size
    finally:
        face = None

def bitmap_bytes_from_image(img, pixel_size):
    bytes_per_row = (pixel_size + 7) // 8
    bitmap_bytes = []
    for row in range(pixel_size):
        for byte_idx in range(bytes_per_row):
            byte_val = 0
            for bit in range(8):
                px = byte_idx * 8 + bit
                if px < pixel_size:
                    pixel = img.getpixel((px, row))
                    if pixel:
                        byte_val |= (1 << (7 - bit))
            bitmap_bytes.append(byte_val)
    return bitmap_bytes

def generate_font_header(font_path, pixel_size, output_path, translations_dir=None, codepoints_file=None,
                         variation_name=None):
    """Generate CJK UI font header file."""

    font, pt_size, ascent, descent = load_font_fitting_cell(font_path, pixel_size, variation_name)
    if font is None:
        return False

    chars = get_unique_chars(BASE_UI_CHARS, translations_dir, codepoints_file)
    print(f"Generating {pixel_size}x{pixel_size} font with {len(chars)} characters...")

    # Collect glyph data
    codepoints = []
    widths = []
    bitmaps = []

    # Get font metrics for consistent vertical alignment
    font_height = ascent + descent
    # Fixed baseline (from top): align all glyphs to the same baseline to avoid jitter
    baseline = pixel_size - descent

    for char in chars:
        cp = ord(char)

        # Create image for character
        img = Image.new('1', (pixel_size, pixel_size), 0)
        draw = ImageDraw.Draw(img)

        # Get character bounding box
        try:
            bbox = font.getbbox(char)
            if bbox:
                char_width = bbox[2] - bbox[0]
                char_height = bbox[3] - bbox[1]
            else:
                char_width = pixel_size // 2
                char_height = pixel_size
        except:
            char_width = pixel_size // 2
            char_height = pixel_size

        # Left align with 1px padding if space allows
        x = 0 if char_width > pixel_size - 2 else 1
        # Align to a fixed baseline to keep CJK/Latin stable within the same line
        y = baseline

        # Draw character
        try:
            # Use left-baseline anchor: y is the baseline position
            draw.text((x, y), char, font=font, fill=1, anchor="ls")
        except TypeError:
            # Fallback for older Pillow: approximate baseline by shifting up
            draw.text((x, y - ascent), char, font=font, fill=1)

        bitmap_bytes = bitmap_bytes_from_image(img, pixel_size)

        codepoints.append(cp)
        # Calculate advance width
        if cp < 0x80:
            # ASCII: use actual width + small padding
            widths.append(min(char_width + 2, pixel_size))
        else:
            # CJK: use full width
            widths.append(pixel_size)
        bitmaps.append(bitmap_bytes)

    vertical_alternates = [
        (0x30FC, CJK_UI_VERTICAL_PROLONGED_SOUND_MARK),
        (0x30A0, CJK_UI_VERTICAL_DOUBLE_HYPHEN),
    ]
    for source_cp, synthetic_cp in vertical_alternates:
        glyph_id = find_vertical_alternate_glyph_id(font_path, source_cp)
        if glyph_id is not None:
            img, width = rasterize_glyph_id(font_path, glyph_id, pixel_size, pt_size, baseline, variation_name)
            codepoints.append(synthetic_cp)
            widths.append(width)
            bitmaps.append(bitmap_bytes_from_image(img, pixel_size))
            print(f"  Added U+{synthetic_cp:04X} vertical alternate for U+{source_cp:04X}")

    glyph_entries = sorted(zip(codepoints, widths, bitmaps), key=lambda item: item[0])
    codepoints = [entry[0] for entry in glyph_entries]
    widths = [entry[1] for entry in glyph_entries]
    bitmaps = [entry[2] for entry in glyph_entries]

    # Generate header file
    bytes_per_row = (pixel_size + 7) // 8
    bytes_per_char = bytes_per_row * pixel_size

    with open(output_path, 'w', encoding="utf-8") as f:
        f.write(f'''/**
 * Auto-generated CJK UI font data (optimized - UI characters only)
 * Font: (see --font argument)
 * Variation: {variation_name or "default"}
 * Size: {pt_size}pt
 * Dimensions: {pixel_size}x{pixel_size}
 * Characters: {len(codepoints)}
 * Total size: {len(codepoints) * bytes_per_char} bytes ({len(codepoints) * bytes_per_char / 1024:.1f} KB)
 *
 * This is a sparse font containing only UI-required CJK characters.
 * Uses a lookup table for codepoint -> glyph index mapping.
 * Supports proportional spacing for English characters.
 */
#pragma once
namespace CjkUiFont{pixel_size} {{

#include <cstdint>
#include <pgmspace.h>

// Font parameters
static constexpr uint8_t CJK_UI_FONT_WIDTH = {pixel_size};
static constexpr uint8_t CJK_UI_FONT_HEIGHT = {pixel_size};
static constexpr uint8_t CJK_UI_FONT_BYTES_PER_ROW = {bytes_per_row};
static constexpr uint8_t CJK_UI_FONT_BYTES_PER_CHAR = {bytes_per_char};
static constexpr uint16_t CJK_UI_FONT_GLYPH_COUNT = {len(codepoints)};

// Codepoint lookup table (sorted for binary search)
static const uint16_t CJK_UI_CODEPOINTS[] PROGMEM = {{
''')

        # Write codepoints
        for i, cp in enumerate(codepoints):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'0x{cp:04X}, ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        if len(codepoints) % 16 != 0:
            f.write('\n')
        f.write('};\n\n')

        # Write widths
        f.write('// Glyph width table (actual advance width for proportional spacing)\n')
        f.write('static const uint8_t CJK_UI_GLYPH_WIDTHS[] PROGMEM = {\n')
        for i, w in enumerate(widths):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'{w:3}, ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        if len(widths) % 16 != 0:
            f.write('\n')
        f.write('};\n\n')

        # Write bitmap data
        f.write('// Glyph bitmap data\n')
        f.write('static const uint8_t CJK_UI_GLYPHS[] PROGMEM = {\n')
        for i, bitmap in enumerate(bitmaps):
            f.write(f'    // U+{codepoints[i]:04X} ({chr(codepoints[i])})\n    ')
            for j, b in enumerate(bitmap):
                f.write(f'0x{b:02X}, ')
                if (j + 1) % 16 == 0 and j < len(bitmap) - 1:
                    f.write('\n    ')
            f.write('\n')
        f.write('};\n\n')

        # Write lookup functions
        f.write('''// Binary search for codepoint
inline int findGlyphIndex(uint16_t codepoint) {
    int low = 0;
    int high = CJK_UI_FONT_GLYPH_COUNT - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint16_t midCp = pgm_read_word(&CJK_UI_CODEPOINTS[mid]);
        if (midCp == codepoint) return mid;
        if (midCp < codepoint) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

inline bool hasCjkUiGlyph(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return false;
    return findGlyphIndex(static_cast<uint16_t>(codepoint)) >= 0;
}

inline const uint8_t* getCjkUiGlyph(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return nullptr;
    int idx = findGlyphIndex(static_cast<uint16_t>(codepoint));
    if (idx < 0) return nullptr;
    return &CJK_UI_GLYPHS[idx * CJK_UI_FONT_BYTES_PER_CHAR];
}

inline uint8_t getCjkUiGlyphWidth(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return 0;
    int idx = findGlyphIndex(static_cast<uint16_t>(codepoint));
    if (idx < 0) return 0;
    return pgm_read_byte(&CJK_UI_GLYPH_WIDTHS[idx]);
}

} // namespace CjkUiFont''' + str(pixel_size) + '\n')

    print(f"Generated: {output_path}")
    print(f"  - {len(codepoints)} characters")
    print(f"  - {len(codepoints) * bytes_per_char} bytes bitmap data")
    return True

def main():
    parser = argparse.ArgumentParser(description='Generate CJK UI font header')
    parser.add_argument('--size', type=int, default=26, help='Pixel size (default: 26)')
    parser.add_argument('--font', type=str, required=True, help='Path to font file (.otf/.ttf)')
    parser.add_argument('--output', type=str, help='Output path (default: lib/GfxRenderer/cjk_ui_font_SIZE.h)')
    parser.add_argument('--translations', type=str,
                        help='Path to translations directory (default: auto-detect from project root)')
    parser.add_argument('--codepoints-file', type=str,
                        help='Additional codepoints file (hex, one per line) to include in the font')
    parser.add_argument('--variation-name', type=str,
                        help='Named variation instance for variable fonts, e.g. Medium or Bold')
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    if args.output:
        output_path = Path(args.output)
    else:
        output_path = project_root / 'lib' / 'GfxRenderer' / f'cjk_ui_font_{args.size}.h'

    if not Path(args.font).exists():
        print(f"Error: Font file not found: {args.font}")
        sys.exit(1)

    # Auto-detect translations directory
    translations_dir = args.translations
    if not translations_dir:
        default_dir = project_root / 'lib' / 'I18n' / 'translations'
        if default_dir.is_dir():
            translations_dir = str(default_dir)
            print(f"Auto-detected translations: {translations_dir}")

    if generate_font_header(args.font, args.size, output_path, translations_dir, args.codepoints_file,
                            args.variation_name):
        print("Success!")
    else:
        print("Failed!")
        sys.exit(1)

if __name__ == '__main__':
    main()
