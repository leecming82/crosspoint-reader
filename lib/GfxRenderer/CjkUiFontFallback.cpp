#include "CjkUiFontFallback.h"

#include "cjk_ui_font_17.h"
#include "cjk_ui_font_19.h"
#include "cjk_ui_font_21.h"

namespace {

constexpr int UI_10_FONT_ID = 22918846;
constexpr int UI_12_FONT_ID = 1635686837;
constexpr int SMALL_FONT_ID = 674098198;

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
    CjkUiFont19::CJK_UI_FONT_WIDTH,
    CjkUiFont19::CJK_UI_FONT_HEIGHT,
    CjkUiFont19::CJK_UI_FONT_BYTES_PER_ROW,
    CjkUiFont19::CJK_UI_FONT_BYTES_PER_CHAR,
    5,
    CjkUiFont19::hasCjkUiGlyph,
    CjkUiFont19::getCjkUiGlyph,
    CjkUiFont19::getCjkUiGlyphWidth,
};

const CjkUiGlyphSet UI12_CJK = {
    CjkUiFont21::CJK_UI_FONT_WIDTH,
    CjkUiFont21::CJK_UI_FONT_HEIGHT,
    CjkUiFont21::CJK_UI_FONT_BYTES_PER_ROW,
    CjkUiFont21::CJK_UI_FONT_BYTES_PER_CHAR,
    7,
    CjkUiFont21::hasCjkUiGlyph,
    CjkUiFont21::getCjkUiGlyph,
    CjkUiFont21::getCjkUiGlyphWidth,
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

bool cjkUiHasAnyGlyph(const uint32_t codepoint) { return CjkUiFont21::hasCjkUiGlyph(codepoint); }
