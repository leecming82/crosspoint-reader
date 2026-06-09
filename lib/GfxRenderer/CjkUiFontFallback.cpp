#include "CjkUiFontFallback.h"

#include "cjk_ui_font_17.h"
#include "cjk_ui_font_21.h"
#include "cjk_ui_font_29.h"

namespace {

constexpr int UI_10_FONT_ID = 22918846;
constexpr int UI_12_FONT_ID = 1635686837;
constexpr int SMALL_FONT_ID = 674098198;

// CJK UI fallback sizes are intentionally tuned separately from the Latin UI fonts.
// The Latin font IDs keep their existing Ubuntu/Noto metrics; these generated CJK
// bitmaps are drawn manually into those line boxes. Larger cells improve Japanese
// legibility, but cost flash and require yOffset tuning so mixed Latin/Japanese
// text aligns visually without clipping compact rows.
const CjkUiGlyphSet SMALL_CJK = {
    CjkUiFont17::CJK_UI_FONT_WIDTH,
    CjkUiFont17::CJK_UI_FONT_HEIGHT,
    CjkUiFont17::CJK_UI_FONT_BYTES_PER_ROW,
    CjkUiFont17::CJK_UI_FONT_BYTES_PER_CHAR,
    6,
    CjkUiFont17::hasCjkUiGlyph,
    CjkUiFont17::getCjkUiGlyph,
    CjkUiFont17::getCjkUiGlyphWidth,
};

const CjkUiGlyphSet UI10_CJK = {
    CjkUiFont21::CJK_UI_FONT_WIDTH,
    CjkUiFont21::CJK_UI_FONT_HEIGHT,
    CjkUiFont21::CJK_UI_FONT_BYTES_PER_ROW,
    CjkUiFont21::CJK_UI_FONT_BYTES_PER_CHAR,
    3,
    CjkUiFont21::hasCjkUiGlyph,
    CjkUiFont21::getCjkUiGlyph,
    CjkUiFont21::getCjkUiGlyphWidth,
};

const CjkUiGlyphSet UI12_CJK = {
    CjkUiFont29::CJK_UI_FONT_WIDTH,
    CjkUiFont29::CJK_UI_FONT_HEIGHT,
    CjkUiFont29::CJK_UI_FONT_BYTES_PER_ROW,
    CjkUiFont29::CJK_UI_FONT_BYTES_PER_CHAR,
    2,
    CjkUiFont29::hasCjkUiGlyph,
    CjkUiFont29::getCjkUiGlyph,
    CjkUiFont29::getCjkUiGlyphWidth,
};

}  // namespace

const CjkUiGlyphSet* cjkUiGlyphSetForFontId(const int fontId) {
  switch (fontId) {
    case SMALL_FONT_ID:
      return &SMALL_CJK;
    case UI_10_FONT_ID:
      return &UI10_CJK;
    case UI_12_FONT_ID:
      return &UI12_CJK;
    default:
      return nullptr;
  }
}

bool cjkUiHasGlyphForFontId(const int fontId, const uint32_t codepoint) {
  if (codepoint < 0x80) return false;
  const CjkUiGlyphSet* set = cjkUiGlyphSetForFontId(fontId);
  return set != nullptr && set->hasGlyph(codepoint);
}

bool cjkUiHasAnyGlyph(const uint32_t codepoint) { return CjkUiFont29::hasCjkUiGlyph(codepoint); }
