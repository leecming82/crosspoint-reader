#include "TtfStb.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace ttf {

StbProbeResult probeStbTruetype(const uint8_t* data, const size_t len) {
  StbProbeResult result{};
  result.sfnt = probeSfnt(data, len);
  if (!result.sfnt.ok) {
    result.error = result.sfnt.error;
    return result;
  }

  const int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0) {
    result.error = "stb could not find font offset";
    return result;
  }

  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font, data, offset)) {
    result.error = "stb could not initialize font";
    return result;
  }

  stbtt_GetFontVMetrics(&font, &result.ascent, &result.descent, &result.lineGap);
  result.glyphA = stbtt_FindGlyphIndex(&font, 'A');
  result.ok = true;
  return result;
}

StbRasterResult rasterizeStbGlyph(const uint8_t* data, const size_t len, const uint32_t codepoint,
                                  const float pixelHeight, uint8_t* bitmap, const size_t bitmapCapacity) {
  StbRasterResult result{};
  result.codepoint = codepoint;
  result.sfnt = probeSfnt(data, len);
  if (!result.sfnt.ok) {
    result.error = result.sfnt.error;
    return result;
  }
  if (!bitmap || bitmapCapacity == 0) {
    result.error = "missing bitmap buffer";
    return result;
  }
  if (pixelHeight <= 0.0f) {
    result.error = "invalid pixel height";
    return result;
  }

  const int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0) {
    result.error = "stb could not find font offset";
    return result;
  }

  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font, data, offset)) {
    result.error = "stb could not initialize font";
    return result;
  }

  result.glyph = stbtt_FindGlyphIndex(&font, static_cast<int>(codepoint));
  if (result.glyph == 0) {
    result.error = "missing glyph";
    return result;
  }

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  const float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);
  stbtt_GetGlyphBitmapBox(&font, result.glyph, scale, scale, &x0, &y0, &x1, &y1);
  result.width = x1 - x0;
  result.height = y1 - y0;
  result.xOffset = x0;
  result.yOffset = y0;
  stbtt_GetGlyphHMetrics(&font, result.glyph, &result.advanceWidth, &result.leftSideBearing);

  if (result.width < 0 || result.height < 0) {
    result.error = "invalid glyph bitmap bounds";
    return result;
  }
  result.bitmapBytes = static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
  if (result.bitmapBytes > bitmapCapacity) {
    result.error = "bitmap buffer too small";
    return result;
  }
  if (result.bitmapBytes > 0) {
    stbtt_MakeGlyphBitmap(&font, bitmap, result.width, result.height, result.width, scale, scale, result.glyph);
  }

  result.ok = true;
  return result;
}

}  // namespace ttf
