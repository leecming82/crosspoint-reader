#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr int SMALL_FONT_ID = 674098198;
constexpr int NOTOSERIF_12_FONT_ID = 85340443;
constexpr int NOTOSERIF_14_FONT_ID = -1367885987;
constexpr int NOTOSERIF_16_FONT_ID = 1428909134;
constexpr int NOTOSERIF_18_FONT_ID = -501438527;
constexpr int NOTOSANS_12_FONT_ID = 2057568286;
constexpr int NOTOSANS_14_FONT_ID = -1589315735;
constexpr int NOTOSANS_16_FONT_ID = 1669013660;
constexpr int NOTOSANS_18_FONT_ID = 37077304;

size_t utf8CodepointCount(const std::string& text) {
  size_t count = 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (utf8NextCodepoint(&ptr)) {
    ++count;
  }
  return count;
}

uint32_t firstCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&ptr);
}

bool isAsciiDigitString(const std::string& text) {
  if (text.empty()) return false;
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    if (!VerticalTextUtils::isAsciiDigit(cp)) return false;
  }
  return true;
}

int rubyFontIdForBodyFont(const int fontId) {
  switch (fontId) {
    case NOTOSERIF_18_FONT_ID:
      return NOTOSERIF_16_FONT_ID;
    case NOTOSERIF_16_FONT_ID:
      return NOTOSERIF_14_FONT_ID;
    case NOTOSERIF_14_FONT_ID:
      return NOTOSERIF_12_FONT_ID;
    case NOTOSANS_18_FONT_ID:
      return NOTOSANS_16_FONT_ID;
    case NOTOSANS_16_FONT_ID:
      return NOTOSANS_14_FONT_ID;
    case NOTOSANS_14_FONT_ID:
      return NOTOSANS_12_FONT_ID;
    case NOTOSERIF_12_FONT_ID:
    case NOTOSANS_12_FONT_ID:
      return SMALL_FONT_ID;
    default:
      // SD-card fonts are currently loaded one effective size at a time. Prefer the
      // small UI/CJK fallback font for ruby and suppress a ruby group if it is not
      // covered, rather than drawing same-size ruby with the body font.
      return SMALL_FONT_ID;
  }
}

int drawableRubyFontId(const GfxRenderer& renderer, const int bodyFontId, const std::string& ruby) {
  const int rubyFontId = rubyFontIdForBodyFont(bodyFontId);
  return renderer.canRenderText(rubyFontId, ruby.c_str(), EpdFontFamily::REGULAR) ? rubyFontId : 0;
}

int rubyReservedHeight(const GfxRenderer& renderer, const int rubyFontId) {
  const int lineHeight = renderer.getLineHeight(rubyFontId);
  return std::max(1, lineHeight - std::max(2, lineHeight / 8));
}

int horizontalRubyYBias(const GfxRenderer& renderer, const int rubyFontId) {
  return std::max(1, std::max(1, renderer.getLineHeight(rubyFontId)) / 4);
}

void drawHorizontalRuby(const GfxRenderer& renderer, const int bodyFontId, const int wordX, const int baseY,
                        const std::string& base, const std::string& ruby, const EpdFontFamily::Style style,
                        const int rubyOffsetX, const int rubyOffsetY) {
  if (ruby.empty()) return;
  const int rubyFontId = drawableRubyFontId(renderer, bodyFontId, ruby);
  if (rubyFontId == 0) return;
  const int baseWidth = renderer.getTextWidth(bodyFontId, base.c_str(), style);
  const int rubyWidth = renderer.getTextWidth(rubyFontId, ruby.c_str(), EpdFontFamily::REGULAR);
  const int rubyX = wordX + (baseWidth - rubyWidth) / 2 + rubyOffsetX;
  const int rubyYOffset = std::max(0, renderer.getLineHeight(rubyFontId) - renderer.getFontAscenderSize(rubyFontId));
  const int rubyY = baseY - rubyReservedHeight(renderer, rubyFontId) + rubyYOffset +
                    horizontalRubyYBias(renderer, rubyFontId) + rubyOffsetY;
  renderer.drawText(rubyFontId, rubyX, rubyY, ruby.c_str(), true, EpdFontFamily::REGULAR);
}

void drawVerticalRuby(const GfxRenderer& renderer, const int bodyFontId, const int wordX, const int wordY,
                      const int baseAdvance, const int columnWidth, const std::string& ruby, const int rubyOffsetX,
                      const int rubyOffsetY, const int contentBottom) {
  if (ruby.empty()) return;
  const int rubyFontId = drawableRubyFontId(renderer, bodyFontId, ruby);
  if (rubyFontId == 0) return;
  const int rubyLineHeight = std::max(1, renderer.getLineHeight(rubyFontId));
  const int rubyX =
      wordX + std::max(0, columnWidth - rubyLineHeight * 5 / 6) + std::max(1, rubyLineHeight / 12) + rubyOffsetX;
  const int rubyAdvance = std::max(1, renderer.getTextAdvanceX(rubyFontId, ruby.c_str(), EpdFontFamily::REGULAR));
  const int span = std::max(1, baseAdvance);
  int rubyY = wordY + (rubyAdvance <= span ? (span - rubyAdvance) / 2 : 0) + rubyOffsetY;
  if (contentBottom > 0) {
    const int overflow = rubyY + rubyAdvance - contentBottom;
    if (overflow > 0) {
      rubyY -= overflow;
    }
  }
  renderer.drawTextVertical(rubyFontId, rubyX, rubyY, ruby.c_str(), true, EpdFontFamily::REGULAR, 0);
}
}  // namespace

bool TextBlock::hasRuby() const {
  for (const auto& ruby : rubyTexts) {
    if (!ruby.empty()) {
      return true;
    }
  }
  return false;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y, const int rubyOffsetX,
                       const int rubyOffsetY, const int contentBottom) const {
  if (vertical) {
    if (words.size() != wordYpos.size() || words.size() != wordStyles.size() ||
        (!wordXpos.empty() && words.size() != wordXpos.size()) ||
        (!rubyTexts.empty() && words.size() != rubyTexts.size()) ||
        (!rubyBaseAdvances.empty() && words.size() != rubyBaseAdvances.size())) {
      LOG_ERR("TXB",
              "Vertical render skipped: size mismatch (words=%u, xpos=%u, ypos=%u, styles=%u, ruby=%u, adv=%u)\n",
              (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordYpos.size(), (uint32_t)wordStyles.size(),
              (uint32_t)rubyTexts.size(), (uint32_t)rubyBaseAdvances.size());
      return;
    }

    const int columnWidth = renderer.getLineHeight(fontId);
    for (size_t i = 0; i < words.size(); ++i) {
      const int wordX = x + (wordXpos.empty() ? 0 : wordXpos[i]);
      const int wordY = y + wordYpos[i];
      const auto style = wordStyles[i];
      const std::string& word = words[i];
      const uint32_t firstCp = firstCodepoint(word);
      const size_t cpCount = utf8CodepointCount(word);

      const bool forceTateChuYoko = (style & EpdFontFamily::TATE_CHU_YOKO) != 0;
      if (forceTateChuYoko || (isAsciiDigitString(word) && cpCount <= 2)) {
        renderer.drawTextTateChuYoko(fontId, wordX, wordY, word.c_str(), true, style, columnWidth);
      } else if (cpCount > 1 && !VerticalTextUtils::isUprightInVertical(firstCp)) {
        renderer.drawTextSideways(fontId, wordX, wordY, word.c_str(), true, style, columnWidth);
      } else if (cpCount == 1) {
        const uint32_t verticalCp = renderer.getVerticalSubstitution(fontId, firstCp, style);
        const std::string verticalGlyph = verticalCp != firstCp ? utf8FromCodepoint(verticalCp) : word;
        renderer.drawText(fontId, wordX + verticalUprightXOffset, wordY, verticalGlyph.c_str(), true, style);
      } else {
        renderer.drawTextVertical(fontId, wordX, wordY, word.c_str(), true, style, 0);
      }
      if (!rubyTexts.empty() && i < rubyTexts.size() && !rubyTexts[i].empty()) {
        const int nextY = (i + 1 < wordYpos.size()) ? wordYpos[i + 1] : wordYpos[i] + columnWidth;
        const int fallbackAdvance = std::max(1, nextY - wordYpos[i]);
        const int baseAdvance =
            (!rubyBaseAdvances.empty() && rubyBaseAdvances[i] > 0) ? rubyBaseAdvances[i] : fallbackAdvance;
        drawVerticalRuby(renderer, fontId, wordX, wordY, baseAdvance, columnWidth, rubyTexts[i], rubyOffsetX,
                         rubyOffsetY, contentBottom);
      }
    }
    return;
  }

  // Focus annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasFocus = !wordFocusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size())) ||
      (!wordWidths.empty() && words.size() != wordWidths.size()) ||
      (!rubyTexts.empty() && words.size() != rubyTexts.size())) {
    LOG_ERR("TXB",
            "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, widths=%u, "
            "ruby=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)wordFocusBoundary.size(), (uint32_t)wordFocusSuffixX.size(), (uint32_t)wordWidths.size(),
            (uint32_t)rubyTexts.size());
    return;
  }

  const int baseY = y;
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(words[i].c_str(), blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = hasFocus ? wordFocusBoundary[i] : 0;

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender.
    int wordY = baseY;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (!rubyTexts.empty() && i < rubyTexts.size() && !rubyTexts[i].empty()) {
      drawHorizontalRuby(renderer, fontId, wordX, baseY, words[i], rubyTexts[i], currentStyle, rubyOffsetX,
                         rubyOffsetY);
    }

    if (boundary > 0) {
      // Focus split: draw bold prefix, then the regular suffix at a pre-computed x offset.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // suffixX is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), words[i].size(), sizeof(boldBuf) - 1});
      memcpy(boldBuf, words[i].c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = wordX + wordFocusSuffixX[i];
      renderer.drawText(fontId, suffixX, wordY, words[i].c_str() + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, wordX, wordY, words[i].c_str(), true, currentStyle, baseDir);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = !wordWidths.empty() && i < wordWidths.size()
                                    ? wordWidths[i]
                                    : renderer.getTextWidth(fontId, w.c_str(), currentStyle, baseDir);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = wordY + ascender + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle, baseDir);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }
  }
}

bool TextBlock::serialize(HalFile& file) const {
  // Focus annotations are optional; vectors are either empty (no splits in this block)
  // or sized in lockstep with words[].
  const bool hasFocus = !wordFocusBoundary.empty();
  const bool hasXPos = !wordXpos.empty();
  if ((hasXPos && words.size() != wordXpos.size()) || (!vertical && !hasXPos && !words.empty()) ||
      words.size() != wordStyles.size() ||
      (hasFocus && (words.size() != wordFocusBoundary.size() || words.size() != wordFocusSuffixX.size())) ||
      (!wordWidths.empty() && words.size() != wordWidths.size()) ||
      (!rubyTexts.empty() && words.size() != rubyTexts.size())) {
    LOG_ERR("TXB",
            "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, widths=%u, "
            "ruby=%u)\n",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(wordFocusBoundary.size()),
            static_cast<uint32_t>(wordFocusSuffixX.size()), static_cast<uint32_t>(wordWidths.size()),
            static_cast<uint32_t>(rubyTexts.size()));
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  serialization::writePod(file, static_cast<uint8_t>(hasXPos ? 1 : 0));
  if (hasXPos) {
    for (auto x : wordXpos) serialization::writePod(file, x);
  }
  for (auto s : wordStyles) serialization::writePod(file, s);
  // Focus block: 1-byte presence flag, followed by per-word vectors only when present.
  // Saves 3 bytes/word when focus reading is disabled or no word on this line was split.
  serialization::writePod(file, static_cast<uint8_t>(hasFocus ? 1 : 0));
  if (hasFocus) {
    for (auto b : wordFocusBoundary) serialization::writePod(file, b);
    for (auto sx : wordFocusSuffixX) serialization::writePod(file, sx);
  }
  serialization::writePod(file, static_cast<uint8_t>(wordWidths.empty() ? 0 : 1));
  if (!wordWidths.empty()) {
    for (auto width : wordWidths) serialization::writePod(file, width);
  }

  const bool hasRuby = this->hasRuby();
  serialization::writePod(file, static_cast<uint8_t>(hasRuby ? 1 : 0));
  if (hasRuby) {
    for (size_t i = 0; i < words.size(); ++i) {
      serialization::writeString(file, i < rubyTexts.size() ? rubyTexts[i] : std::string());
    }
  }

  serialization::writePod(file, static_cast<uint8_t>(vertical ? 1 : 0));
  if (vertical) {
    serialization::writePod(file, verticalUprightXOffset);
    for (size_t i = 0; i < words.size(); ++i) {
      const int16_t ypos = i < wordYpos.size() ? wordYpos[i] : 0;
      serialization::writePod(file, ypos);
    }
    serialization::writePod(file, static_cast<uint8_t>(rubyBaseAdvances.empty() ? 0 : 1));
    if (!rubyBaseAdvances.empty()) {
      for (size_t i = 0; i < words.size(); ++i) {
        const uint16_t advance = i < rubyBaseAdvances.size() ? rubyBaseAdvances[i] : 0;
        serialization::writePod(file, advance);
      }
    }
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> wordFocusBoundary;
  std::vector<uint16_t> wordFocusSuffixX;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  uint8_t hasXPos;
  serialization::readPod(file, hasXPos);
  if (hasXPos) {
    wordXpos.resize(wc);
    for (auto& x : wordXpos) serialization::readPod(file, x);
  }
  for (auto& s : wordStyles) serialization::readPod(file, s);
  // Focus block: presence flag, then vectors only if present. Empty vectors when absent
  // signal "no splits in this block" to render() (zero per-word RAM cost).
  uint8_t hasFocus;
  serialization::readPod(file, hasFocus);
  if (hasFocus) {
    wordFocusBoundary.resize(wc);
    wordFocusSuffixX.resize(wc);
    for (auto& b : wordFocusBoundary) serialization::readPod(file, b);
    for (auto& sx : wordFocusSuffixX) serialization::readPod(file, sx);
  }
  uint8_t hasWordWidths = 0;
  serialization::readPod(file, hasWordWidths);
  std::vector<uint16_t> wordWidths;
  if (hasWordWidths) {
    wordWidths.resize(wc);
    for (auto& width : wordWidths) serialization::readPod(file, width);
  }
  uint8_t hasRuby;
  serialization::readPod(file, hasRuby);
  std::vector<std::string> rubyTexts;
  if (hasRuby) {
    rubyTexts.resize(wc);
    for (auto& ruby : rubyTexts) serialization::readString(file, ruby);
  }
  uint8_t vertical = 0;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  std::vector<uint16_t> rubyBaseAdvances;
  int16_t verticalUprightXOffset = 0;
  if (vertical) {
    serialization::readPod(file, verticalUprightXOffset);
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
    uint8_t hasRubyAdvances = 0;
    serialization::readPod(file, hasRubyAdvances);
    if (hasRubyAdvances) {
      rubyBaseAdvances.resize(wc);
      for (auto& advance : rubyBaseAdvances) serialization::readPod(file, advance);
    }
  }

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);

  return std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), std::move(wordFocusBoundary),
                    std::move(wordFocusSuffixX), blockStyle, std::move(rubyTexts), std::move(wordYpos), vertical != 0,
                    std::move(rubyBaseAdvances), verticalUprightXOffset, std::move(wordWidths)));
}
