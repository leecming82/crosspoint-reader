#pragma once

#include <cstdint>

struct CjkUiGlyphSet {
  uint8_t cellWidth;
  uint8_t cellHeight;
  uint8_t bytesPerRow;
  uint8_t bytesPerChar;
  int8_t yOffset;
  bool (*hasGlyph)(uint32_t codepoint);
  const uint8_t* (*getGlyph)(uint32_t codepoint);
  uint8_t (*getGlyphWidth)(uint32_t codepoint);
};

const CjkUiGlyphSet* cjkUiGlyphSetForFontId(int fontId);
bool cjkUiHasGlyphForFontId(int fontId, uint32_t codepoint);
bool cjkUiHasAnyGlyph(uint32_t codepoint);
