#include "ParsedText.h"

#include <Arduino.h>
#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr size_t MIN_FREE_HEAP_AFTER_HORIZONTAL_CJK_LAYOUT = 24 * 1024;
constexpr size_t HORIZONTAL_CJK_ALLOC_SAFETY_MARGIN = 4 * 1024;
constexpr size_t MAX_HORIZONTAL_CJK_LAYOUT_UNITS = 1024;
constexpr size_t HORIZONTAL_CJK_LAYOUT_CHUNK_TARGET_UNITS = 768;
constexpr size_t MAX_TATEGAKI_LAYOUT_UNITS = 768;
constexpr size_t TATEGAKI_LAYOUT_CHUNK_TARGET_UNITS = 512;

struct HorizontalCjkUnit {
  uint32_t firstCp;
  uint32_t lastCp;
  uint16_t wordIndex;
  uint16_t startByte;
  uint16_t endByte;
  uint16_t width;
  bool noGapBefore;
  bool noBreakBefore;
  bool isCjk;
  EpdFontFamily::Style style;
};

struct TategakiUnit {
  uint16_t wordIndex;
  uint16_t startByte;
  uint16_t endByte;
  uint16_t advance;
  bool noBreakBefore;
  bool firstUnitInWord;
  bool isKinsokuHead;
  bool isKinsokuTail;
  EpdFontFamily::Style style;
};

// Horizontal CJK/tategaki layout is chunked before these temporary vectors are built
// (1024/768 layout units respectively), so 16-bit source indexes and byte
// spans are enough while avoiding size_t padding in every unit.
static_assert(MAX_HORIZONTAL_CJK_LAYOUT_UNITS <= UINT16_MAX, "Horizontal CJK layout unit indexes must fit in uint16_t");
static_assert(MAX_TATEGAKI_LAYOUT_UNITS <= UINT16_MAX, "Tategaki layout unit indexes must fit in uint16_t");
static_assert(sizeof(HorizontalCjkUnit) <= 20, "HorizontalCjkUnit should stay compact");
static_assert(sizeof(TategakiUnit) <= 14, "TategakiUnit should stay compact");

bool hasHeapForHorizontalCjkLayout(const size_t estimatedBytes) {
  return ESP.getMaxAllocHeap() >= estimatedBytes + HORIZONTAL_CJK_ALLOC_SAFETY_MARGIN &&
         ESP.getFreeHeap() >= estimatedBytes + MIN_FREE_HEAP_AFTER_HORIZONTAL_CJK_LAYOUT;
}

bool hasLatinLetter(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    if (((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)) {
      return true;
    }
  }
  return false;
}

bool isVerticalSidewaysPhraseConnector(const uint32_t cp) {
  switch (cp) {
    case 0x0020:  // space
    case 0x0027:  // apostrophe
    case 0x002D:  // hyphen-minus
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:  // horizontal bar
    case 0xFF0D:  // fullwidth hyphen-minus
      return true;
    default:
      return false;
  }
}

bool isVerticalSidewaysPhraseToken(const std::string& word) {
  if (word.empty()) return false;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    if (isVerticalSidewaysPhraseConnector(cp)) {
      continue;
    }
    if (VerticalTextUtils::isUprightInVertical(cp) || VerticalTextUtils::isAsciiDigit(cp)) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> sliceRubyTexts(const std::vector<std::string>& rubyTexts, const size_t start,
                                        const size_t end) {
  if (rubyTexts.empty()) {
    return {};
  }

  std::vector<std::string> out;
  out.reserve(end - start);
  bool hasRuby = false;
  for (size_t i = start; i < end; ++i) {
    std::string ruby = i < rubyTexts.size() ? rubyTexts[i] : std::string();
    hasRuby = hasRuby || !ruby.empty();
    out.push_back(std::move(ruby));
  }
  return hasRuby ? out : std::vector<std::string>{};
}

// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool canEndVerticalChunkAfter(const std::string& word, const size_t nextOffset, const uint32_t cp) {
  if (VerticalTextUtils::isKinsokuTail(cp)) {
    return false;
  }

  if (nextOffset >= word.size()) {
    return true;
  }

  const auto* nextPtr = reinterpret_cast<const unsigned char*>(word.c_str() + nextOffset);
  const uint32_t nextCp = utf8NextCodepoint(&nextPtr);
  if (nextCp == 0) {
    return true;
  }

  if (VerticalTextUtils::isAsciiDigit(cp) && VerticalTextUtils::isAsciiDigit(nextCp)) {
    return false;
  }

  return VerticalTextUtils::isUprightInVertical(cp) || VerticalTextUtils::isAsciiDigit(cp) ||
         VerticalTextUtils::isUprightInVertical(nextCp) || VerticalTextUtils::isAsciiDigit(nextCp);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

bool isCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK symbols and punctuation
         (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||  // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK unified ideographs
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||  // Fullwidth forms
         (cp >= 0x20000 && cp <= 0x2FA1F);  // CJK extensions B-F + compatibility supplement
}

bool isParagraphIndentSpace(const uint32_t cp) { return cp == 0x0020 || cp == 0x2003 || cp == 0x3000; }

bool startsWithParagraphIndentSpace(const std::string& word) {
  if (word.empty()) {
    return false;
  }

  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const uint32_t cp = utf8NextCodepoint(&ptr);
  return isParagraphIndentSpace(cp);
}

bool isCjkOpeningPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x0028:  // (
    case 0x005B:  // [
    case 0x007B:  // {
    case 0x2018:
    case 0x201C:
    case 0x3008:
    case 0x300A:
    case 0x300C:
    case 0x300E:
    case 0x3010:
    case 0x3014:
    case 0x3016:
    case 0x3018:
    case 0x301A:
    case 0x301D:
    case 0xFF08:
    case 0xFF3B:
    case 0xFF5B:
    case 0xFF5F:
      return true;
    default:
      return false;
  }
}

bool startsWithCjkOpeningPunctuation(const std::string& word) { return isCjkOpeningPunctuation(firstCodepoint(word)); }

bool startsWithCjkOpeningPunctuationInCjkText(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const uint32_t firstCp = utf8NextCodepoint(&ptr);
  if (!isCjkOpeningPunctuation(firstCp)) {
    return false;
  }

  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    if (cp == 0x00AD || cp <= 0x20 || isParagraphIndentSpace(cp)) {
      continue;
    }
    if (isCjkCodepoint(cp)) {
      return true;
    }
  }
  return false;
}

bool removeLeadingIndentBeforeCjkOpeningPunctuation(std::string& word) {
  if (word.empty()) {
    return false;
  }

  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* firstStart = ptr;
  const uint32_t firstCp = utf8NextCodepoint(&ptr);
  if (!isParagraphIndentSpace(firstCp)) {
    return false;
  }

  const auto* secondStart = ptr;
  const uint32_t secondCp = utf8NextCodepoint(&ptr);
  if (!isCjkOpeningPunctuation(secondCp)) {
    return false;
  }

  bool hasCjkText = false;
  while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
    if (cp == 0x00AD || cp <= 0x20 || isParagraphIndentSpace(cp)) {
      continue;
    }
    if (isCjkCodepoint(cp)) {
      hasCjkText = true;
      break;
    }
  }
  if (!hasCjkText) {
    return false;
  }

  word.erase(0, static_cast<size_t>(secondStart - firstStart));
  return true;
}

bool isCjkNoLineStart(const uint32_t cp) {
  switch (cp) {
    case 0x0021:  // !
    case 0x0025:  // %
    case 0x0029:  // )
    case 0x002C:  // ,
    case 0x002E:  // .
    case 0x003F:  // ?
    case 0x005D:  // ]
    case 0x007D:  // }
    case 0x00A2:
    case 0x00B0:
    case 0x2019:
    case 0x201D:
    case 0x2026:
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3005:
    case 0x3009:
    case 0x300B:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x3015:
    case 0x3017:
    case 0x3019:
    case 0x301B:
    case 0x301F:
    case 0x3041:
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:
    case 0x3063:
    case 0x3083:
    case 0x3085:
    case 0x3087:
    case 0x308E:
    case 0x309D:
    case 0x309E:
    case 0x30A1:
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:
    case 0x30C3:
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:
    case 0x30EE:
    case 0x30F5:
    case 0x30F6:
    case 0x30FC:
    case 0x30FD:
    case 0x30FE:
    case 0xFF01:
    case 0xFF05:
    case 0xFF09:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1F:
    case 0xFF3D:
    case 0xFF5D:
    case 0xFF60:
    case 0xFFE0:
      return true;
    default:
      return false;
  }
}

bool isCjkNoLineEnd(const uint32_t cp) { return isCjkOpeningPunctuation(cp); }

bool isCjkClosingPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x0021:  // !
    case 0x0025:  // %
    case 0x0029:  // )
    case 0x002C:  // ,
    case 0x002E:  // .
    case 0x003F:  // ?
    case 0x005D:  // ]
    case 0x007D:  // }
    case 0x2019:
    case 0x201D:
    case 0x2026:
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:
    case 0x300B:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x3015:
    case 0x3017:
    case 0x3019:
    case 0x301B:
    case 0x301F:
    case 0xFF01:
    case 0xFF05:
    case 0xFF09:
    case 0xFF0C:
    case 0xFF0E:
    case 0xFF1F:
    case 0xFF3D:
    case 0xFF5D:
    case 0xFF60:
      return true;
    default:
      return false;
  }
}

bool isCjkChunkBoundary(const uint32_t cp) {
  return cp == 0x0021 || cp == 0x002E || cp == 0x003F ||  // ! . ?
         cp == 0x3002 || cp == 0xFF01 || cp == 0xFF0E || cp == 0xFF1F;
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

uint16_t measureRunWidth(const GfxRenderer& renderer, const int fontId, const std::string& run,
                         const EpdFontFamily::Style style) {
  const int advanceWidth = measureWordWidth(renderer, fontId, run, style);
  const int boundsWidth = renderer.getTextWidth(fontId, run.c_str(), style);
  return static_cast<uint16_t>(std::max(advanceWidth, boundsWidth));
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const bool tateChuYoko) {
  if (word.empty()) return;

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  if (tateChuYoko) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::TATE_CHU_YOKO);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (tateChuYoko || !this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    words.push_back(std::move(word));
    wordStyles.push_back(baseStyle);
    wordContinues.push_back(attachToPrevious);
    wordIsFocusSuffix.push_back(false);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Pre-reserve capacity to prevent mid-word heap reallocations.
  size_t maxPossibleNewTokens = word.length();
  size_t requiredSize = words.size() + maxPossibleNewTokens;

  if (words.capacity() < requiredSize) {
    // Emulate standard geometric growth (doubling) to ensure we don't reallocate on every word.
    size_t newCapacity = words.capacity() * 2;

    // Ensure the doubled capacity is actually enough for this specific word
    if (newCapacity < requiredSize) {
      newCapacity = requiredSize;
    }
    // Set a sensible minimum starting size so the first few words don't trigger tiny reallocations
    if (newCapacity < 16) {
      newCapacity = 16;
    }

    words.reserve(newCapacity);
    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordIsFocusSuffix.reserve(newCapacity);
  }

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach) {
    if (!isWord) {
      // Punctuation and Numbers stay regular
      words.emplace_back(segment);
      wordStyles.push_back(baseStyle);
      wordContinues.push_back(attach);
      wordIsFocusSuffix.push_back(false);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        words.emplace_back(segment);
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordIsFocusSuffix.push_back(false);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // Bold prefix
        words.emplace_back(segment.substr(0, splitByteOffset));
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordIsFocusSuffix.push_back(false);

        // Regular suffix - marked so extractLine can merge it back into single TextBlock entry
        words.emplace_back(segment.substr(splitByteOffset));
        wordStyles.push_back(baseStyle);
        wordContinues.push_back(true);
        wordIsFocusSuffix.push_back(true);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment MUST attach=true so it glues seamlessly to the prefix.
      processSegment(segment, inWordSegment, isFirstSegment ? attachToPrevious : true);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  processSegment(segment, inWordSegment, isFirstSegment ? attachToPrevious : true);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine) const {
  if (isFirstLine && blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
      isNaturalAlign) {
    return blockStyle.textIndent;
  }
  return 0;
}

void ParsedText::addRubyWord(std::string word, std::string ruby, const EpdFontFamily::Style fontStyle,
                             const bool underline, const bool attachToPrevious) {
  if (word.empty()) return;

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }

  // Keep ruby bases as a single logical token. That avoids the focus-reading splitter
  // putting the annotation on only the bold prefix of a Japanese word.
  words.push_back(std::move(word));
  wordStyles.push_back(baseStyle);
  wordContinues.push_back(attachToPrevious);
  wordIsFocusSuffix.push_back(false);

  if (!ruby.empty()) {
    if (rubyTexts.size() < words.size()) {
      rubyTexts.resize(words.size());
    }
    rubyTexts.back() = std::move(ruby);
  }
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine, const bool sdAdvancePrewarmed) {
  if (words.empty()) {
    return;
  }

  const bool useHorizontalCjkWrapper = shouldUseHorizontalCjkWrapper();

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For reader fonts that need preparation, this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the reader font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    if (!sdAdvancePrewarmed) {
      renderer.ensureReaderFontReady(fontId, words, hyphenationEnabled, styleMask);
      if (useHorizontalCjkWrapper) {
        renderer.ensureReaderFontReady(fontId, "\xe6\x97\xa5\xe3\x81\x82", styleMask);  // 日あ
      }
    }
  }

  const int pageWidth = viewportWidth;
  if (useHorizontalCjkWrapper) {
    if (layoutAndExtractHorizontalCjkLines(renderer, fontId, pageWidth, processLine, includeLastLine,
                                           sdAdvancePrewarmed)) {
      return;
    }
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordIsFocusSuffix.erase(wordIsFocusSuffix.begin(), wordIsFocusSuffix.begin() + consumed);
    if (!rubyTexts.empty()) {
      const size_t rubyConsumed = std::min(consumed, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rubyConsumed);
    }
  }
}

void ParsedText::layoutAndExtractVerticalColumns(const GfxRenderer& renderer, const int fontId,
                                                 const uint16_t columnHeight,
                                                 const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                                                 const bool sdAdvancePrewarmed, const bool includeLastColumn) {
  if (words.empty()) {
    return;
  }

  if (includeLastColumn &&
      layoutAndExtractChunkedTategakiColumns(renderer, fontId, columnHeight, processColumn, sdAdvancePrewarmed)) {
    return;
  }

  if (renderer.isSdCardFont(fontId)) {
    uint8_t styleMask = 0;
    for (auto style : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;
    if (!sdAdvancePrewarmed) {
      renderer.ensureReaderFontReady(fontId, words, /*includeHyphenGlyph=*/false, styleMask);
    }
  }

  const int lineHeight = std::max(1, renderer.getLineHeight(fontId));
  const int representativeCjkAdvance =
      std::max(renderer.getTextAdvanceX(fontId, "\xe6\x97\xa5", EpdFontFamily::REGULAR),   // 日
               renderer.getTextAdvanceX(fontId, "\xe3\x81\x82", EpdFontFamily::REGULAR));  // あ
  const int cjkCellAdvance = std::max(1, representativeCjkAdvance > 0 ? representativeCjkAdvance : lineHeight * 9 / 10);
  const int16_t uprightGlyphXOffset =
      representativeCjkAdvance > 0 ? static_cast<int16_t>(std::max(0, (lineHeight - representativeCjkAdvance) / 2)) : 0;

  std::vector<TategakiUnit> units;
  units.reserve(words.size() * 2);

  auto pushUnit = [&](const uint16_t wordIndex, const uint16_t start, const uint16_t end, const uint32_t firstCp,
                      const uint32_t lastCp, const int advance, const bool noBreakBefore, const bool firstUnitInWord) {
    units.push_back({wordIndex, start, end, static_cast<uint16_t>(std::max(1, advance)), noBreakBefore, firstUnitInWord,
                     VerticalTextUtils::isKinsokuHead(firstCp), VerticalTextUtils::isKinsokuTail(lastCp),
                     wordStyles[wordIndex]});
  };

  for (uint16_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const std::string& word = words[wordIndex];
    if ((wordStyles[wordIndex] & EpdFontFamily::TATE_CHU_YOKO) != 0) {
      const uint32_t firstCp = firstCodepoint(word);
      const uint32_t lastCp = lastCodepoint(word);
      pushUnit(wordIndex, 0, static_cast<uint16_t>(word.size()), firstCp, lastCp, cjkCellAdvance,
               wordContinues[wordIndex], true);
      continue;
    }

    uint16_t offset = 0;
    bool firstUnitInWord = true;
    while (offset < word.size()) {
      const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) break;
      const uint16_t nextOffset = static_cast<uint16_t>(reinterpret_cast<const char*>(ptr) - word.c_str());

      if (VerticalTextUtils::isAsciiDigit(cp)) {
        uint16_t runEnd = nextOffset;
        uint32_t lastCpInRun = cp;
        size_t digitCount = 1;
        while (runEnd < word.size() && digitCount < 2) {
          const auto* lookaheadPtr = reinterpret_cast<const unsigned char*>(word.c_str() + runEnd);
          const uint32_t lookaheadCp = utf8NextCodepoint(&lookaheadPtr);
          if (!VerticalTextUtils::isAsciiDigit(lookaheadCp)) break;
          lastCpInRun = lookaheadCp;
          runEnd = static_cast<uint16_t>(reinterpret_cast<const char*>(lookaheadPtr) - word.c_str());
          ++digitCount;
        }
        pushUnit(wordIndex, offset, runEnd, cp, lastCpInRun, cjkCellAdvance,
                 firstUnitInWord && wordContinues[wordIndex], firstUnitInWord);
        offset = runEnd;
        firstUnitInWord = false;
        continue;
      }

      if (!VerticalTextUtils::isUprightInVertical(cp)) {
        uint16_t runEnd = nextOffset;
        uint32_t lastCpInRun = cp;
        while (runEnd < word.size()) {
          const auto* lookaheadPtr = reinterpret_cast<const unsigned char*>(word.c_str() + runEnd);
          const uint32_t lookaheadCp = utf8NextCodepoint(&lookaheadPtr);
          if (lookaheadCp == 0 || VerticalTextUtils::isUprightInVertical(lookaheadCp) ||
              VerticalTextUtils::isAsciiDigit(lookaheadCp)) {
            break;
          }
          lastCpInRun = lookaheadCp;
          runEnd = static_cast<uint16_t>(reinterpret_cast<const char*>(lookaheadPtr) - word.c_str());
        }
        const std::string run = word.substr(offset, runEnd - offset);
        pushUnit(wordIndex, offset, runEnd, cp, lastCpInRun,
                 measureRunWidth(renderer, fontId, run, wordStyles[wordIndex]),
                 firstUnitInWord && wordContinues[wordIndex], firstUnitInWord);
        offset = runEnd;
        firstUnitInWord = false;
        continue;
      }

      char cpText[5] = {};
      const size_t cpBytes = nextOffset - offset;
      for (size_t i = 0; i < cpBytes && i < 4; ++i) cpText[i] = word[offset + i];
      const uint32_t verticalCp = renderer.getVerticalSubstitution(fontId, cp, wordStyles[wordIndex]);
      const std::string verticalCpText = verticalCp != cp ? utf8FromCodepoint(verticalCp) : std::string();
      const char* measuredText = verticalCp != cp ? verticalCpText.c_str() : cpText;
      const int measured = renderer.getTextAdvanceX(fontId, measuredText, wordStyles[wordIndex]);
      pushUnit(wordIndex, offset, nextOffset, cp, cp, std::max(cjkCellAdvance, measured),
               firstUnitInWord && wordContinues[wordIndex], firstUnitInWord);
      offset = nextOffset;
      firstUnitInWord = false;
    }
  }
  if (units.empty()) {
    words.clear();
    wordStyles.clear();
    wordContinues.clear();
    wordIsFocusSuffix.clear();
    rubyTexts.clear();
    return;
  }

  auto canBreakAfter = [&units](const size_t index) {
    if (index + 1 >= units.size()) return true;
    if (units[index + 1].noBreakBefore) return false;
    return !units[index].isKinsokuTail && !units[index + 1].isKinsokuHead;
  };

  auto emitColumn = [&](const size_t start, const size_t end) {
    std::vector<std::string> columnWords;
    std::vector<int16_t> columnYPos;
    std::vector<EpdFontFamily::Style> columnStyles;
    std::vector<std::string> columnRubyTexts;
    std::vector<uint16_t> columnRubyBaseAdvances;
    columnWords.reserve(end - start);
    columnYPos.reserve(end - start);
    columnStyles.reserve(end - start);

    auto unitText = [&](const TategakiUnit& unit) {
      const std::string& source = words[unit.wordIndex];
      return source.substr(unit.startByte, unit.endByte - unit.startByte);
    };

    auto unitRuby = [&](const TategakiUnit& unit) {
      if (unit.firstUnitInWord && unit.wordIndex < rubyTexts.size()) {
        return rubyTexts[unit.wordIndex];
      }
      return std::string();
    };

    auto pushRubySidecar = [&](std::string ruby, const uint16_t rubyBaseAdvance) {
      if (ruby.empty() && columnRubyTexts.empty()) {
        return;
      }
      if (columnRubyTexts.empty()) {
        columnRubyTexts.reserve(end - start);
        columnRubyBaseAdvances.reserve(end - start);
        columnRubyTexts.resize(columnWords.size() - 1);
        columnRubyBaseAdvances.resize(columnWords.size() - 1);
      }
      columnRubyTexts.push_back(std::move(ruby));
      columnRubyBaseAdvances.push_back(rubyBaseAdvance);
    };

    int ypos = 0;
    for (size_t i = start; i < end;) {
      const TategakiUnit& unit = units[i];
      std::string text = unitText(unit);
      std::string ruby = unitRuby(unit);
      const bool canStartSidewaysPhrase = ruby.empty() && (unit.style & EpdFontFamily::TATE_CHU_YOKO) == 0 &&
                                          isVerticalSidewaysPhraseToken(text) && hasLatinLetter(text);

      if (canStartSidewaysPhrase) {
        int phraseAdvance = unit.advance;
        size_t next = i + 1;
        while (next < end) {
          const TategakiUnit& nextUnit = units[next];
          std::string nextText = unitText(nextUnit);
          if (!unitRuby(nextUnit).empty() || nextUnit.style != unit.style ||
              (nextUnit.style & EpdFontFamily::TATE_CHU_YOKO) != 0 || !isVerticalSidewaysPhraseToken(nextText)) {
            break;
          }
          text += nextText;
          phraseAdvance += nextUnit.advance;
          ++next;
        }

        columnWords.push_back(std::move(text));
        columnYPos.push_back(static_cast<int16_t>(ypos));
        columnStyles.push_back(unit.style);
        pushRubySidecar(std::string(), 0);
        ypos += phraseAdvance;
        i = next;
        continue;
      }

      columnWords.push_back(std::move(text));
      columnYPos.push_back(static_cast<int16_t>(ypos));
      columnStyles.push_back(unit.style);
      uint16_t rubyBaseAdvance = 0;
      if (!ruby.empty()) {
        int spanAdvance = 0;
        for (size_t j = i; j < end && units[j].wordIndex == unit.wordIndex; ++j) {
          spanAdvance += units[j].advance;
        }
        rubyBaseAdvance = static_cast<uint16_t>(std::min<int>(std::max(1, spanAdvance), UINT16_MAX));
      }
      pushRubySidecar(std::move(ruby), rubyBaseAdvance);
      ypos += unit.advance;
      ++i;
    }

    processColumn(std::make_shared<TextBlock>(std::move(columnWords), std::vector<int16_t>{}, std::move(columnStyles),
                                              std::vector<uint8_t>{}, std::vector<uint16_t>{}, blockStyle,
                                              std::move(columnRubyTexts), std::move(columnYPos), true,
                                              std::move(columnRubyBaseAdvances), uprightGlyphXOffset));
  };

  size_t columnStart = 0;
  std::vector<size_t> columnEnds;
  while (columnStart < units.size()) {
    int columnUsed = 0;
    size_t bestBreak = columnStart;
    size_t i = columnStart;
    for (; i < units.size(); ++i) {
      const int candidate = columnUsed + units[i].advance;
      if (candidate > columnHeight && i > columnStart) {
        break;
      }
      columnUsed = candidate;
      if (canBreakAfter(i)) {
        bestBreak = i + 1;
      }
    }

    size_t columnEnd = i >= units.size() ? units.size() : bestBreak;
    if (columnEnd <= columnStart) {
      columnEnd = columnStart + 1;
    }
    columnEnds.push_back(columnEnd);
    columnStart = columnEnd;
  }

  size_t emitColumnCount = columnEnds.size();
  if (!includeLastColumn && emitColumnCount > 0) {
    --emitColumnCount;
  }

  size_t emitStart = 0;
  for (size_t columnIndex = 0; columnIndex < emitColumnCount; ++columnIndex) {
    emitColumn(emitStart, columnEnds[columnIndex]);
    emitStart = columnEnds[columnIndex];
  }

  if (emitStart >= units.size()) {
    words.clear();
    wordStyles.clear();
    wordContinues.clear();
    wordIsFocusSuffix.clear();
    rubyTexts.clear();
    return;
  }

  std::vector<std::string> remainingWords;
  std::vector<EpdFontFamily::Style> remainingStyles;
  std::vector<bool> remainingContinues;
  std::vector<std::string> remainingRubyTexts;
  remainingWords.reserve(units.size() - emitStart);
  remainingStyles.reserve(units.size() - emitStart);
  remainingContinues.reserve(units.size() - emitStart);
  bool hasRemainingRuby = false;
  size_t lastWordIndex = std::numeric_limits<size_t>::max();
  size_t lastEndByte = 0;

  for (size_t i = emitStart; i < units.size(); ++i) {
    const TategakiUnit& unit = units[i];
    const std::string& source = words[unit.wordIndex];
    if (!remainingWords.empty() && unit.wordIndex == lastWordIndex && unit.startByte == lastEndByte &&
        unit.style == remainingStyles.back()) {
      remainingWords.back().append(source, unit.startByte, unit.endByte - unit.startByte);
      lastEndByte = unit.endByte;
      continue;
    }

    remainingWords.emplace_back(source, unit.startByte, unit.endByte - unit.startByte);
    remainingStyles.push_back(unit.style);
    remainingContinues.push_back(!remainingContinues.empty() && (unit.noBreakBefore || !unit.firstUnitInWord));
    std::string ruby;
    if (unit.firstUnitInWord && unit.wordIndex < rubyTexts.size()) {
      ruby = rubyTexts[unit.wordIndex];
    }
    hasRemainingRuby = hasRemainingRuby || !ruby.empty();
    if (hasRemainingRuby || !remainingRubyTexts.empty()) {
      if (remainingRubyTexts.size() + 1 < remainingWords.size()) {
        remainingRubyTexts.resize(remainingWords.size() - 1);
      }
      remainingRubyTexts.push_back(std::move(ruby));
    }
    lastWordIndex = unit.wordIndex;
    lastEndByte = unit.endByte;
  }

  words = std::move(remainingWords);
  wordStyles = std::move(remainingStyles);
  wordContinues = std::move(remainingContinues);
  wordIsFocusSuffix.assign(words.size(), false);
  rubyTexts = hasRemainingRuby ? std::move(remainingRubyTexts) : std::vector<std::string>{};
}

bool ParsedText::layoutAndExtractChunkedTategakiColumns(
    const GfxRenderer& renderer, const int fontId, const uint16_t columnHeight,
    const std::function<void(std::shared_ptr<TextBlock>)>& processColumn, const bool sdAdvancePrewarmed) {
  struct Chunk {
    std::vector<std::string> words;
    std::vector<EpdFontFamily::Style> styles;
    std::vector<bool> continues;
    std::vector<std::string> rubyTexts;
    size_t units = 0;
  };

  size_t totalUnits = 0;
  for (const auto& word : words) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
    while (utf8NextCodepoint(&ptr)) {
      ++totalUnits;
      if (totalUnits > MAX_TATEGAKI_LAYOUT_UNITS) {
        break;
      }
    }
    if (totalUnits > MAX_TATEGAKI_LAYOUT_UNITS) {
      break;
    }
  }
  if (totalUnits <= MAX_TATEGAKI_LAYOUT_UNITS) {
    return false;
  }

  auto sourceWords = std::move(words);
  auto sourceStyles = std::move(wordStyles);
  auto sourceContinues = std::move(wordContinues);
  auto sourceRubyTexts = std::move(rubyTexts);
  Chunk current;

  const int lineHeight = std::max(1, renderer.getLineHeight(fontId));
  const int representativeCjkAdvance =
      std::max(renderer.getTextAdvanceX(fontId, "\xe6\x97\xa5", EpdFontFamily::REGULAR),   // 日
               renderer.getTextAdvanceX(fontId, "\xe3\x81\x82", EpdFontFamily::REGULAR));  // あ
  const int cjkCellAdvance = std::max(1, representativeCjkAdvance > 0 ? representativeCjkAdvance : lineHeight * 9 / 10);
  const size_t estimatedUnitsPerColumn = std::max<size_t>(1, static_cast<size_t>(columnHeight) / cjkCellAdvance);
  const size_t preferredChunkUnits =
      std::max(estimatedUnitsPerColumn,
               (TATEGAKI_LAYOUT_CHUNK_TARGET_UNITS / estimatedUnitsPerColumn) * estimatedUnitsPerColumn);

  auto finishChunk = [&]() {
    if (current.words.empty()) {
      return;
    }

    words = std::move(current.words);
    wordStyles = std::move(current.styles);
    wordContinues = std::move(current.continues);
    wordIsFocusSuffix.assign(words.size(), false);
    rubyTexts = std::move(current.rubyTexts);
    current = Chunk();
    layoutAndExtractVerticalColumns(renderer, fontId, columnHeight, processColumn, sdAdvancePrewarmed,
                                    /*includeLastColumn=*/false);
    if (!words.empty()) {
      current.words = std::move(words);
      current.styles = std::move(wordStyles);
      current.continues = std::move(wordContinues);
      current.rubyTexts = std::move(rubyTexts);
      current.units = 0;
      for (const auto& word : current.words) {
        const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
        while (utf8NextCodepoint(&ptr)) {
          ++current.units;
        }
      }
      wordIsFocusSuffix.clear();
    }
  };

  auto addSegment = [&](const size_t wordIndex, const size_t start, const size_t end, const EpdFontFamily::Style style,
                        const bool attachToPrevious, const size_t unitCount, const std::string& ruby) {
    std::string& word = sourceWords[wordIndex];
    if (start >= end) {
      return;
    }
    const bool wholeWord = start == 0 && end == word.size();
    if (wholeWord) {
      current.words.push_back(std::move(word));
    } else {
      current.words.emplace_back(word, start, end - start);
    }
    current.styles.push_back(style);
    current.continues.push_back(current.words.size() > 1 && attachToPrevious);
    if (!ruby.empty() || !current.rubyTexts.empty()) {
      if (current.rubyTexts.size() + 1 < current.words.size()) {
        current.rubyTexts.resize(current.words.size() - 1);
      }
      current.rubyTexts.push_back(wholeWord ? ruby : std::string());
    }
    current.units += unitCount;
  };

  for (size_t wordIndex = 0; wordIndex < sourceWords.size(); ++wordIndex) {
    const std::string& word = sourceWords[wordIndex];
    const std::string emptyRuby;
    const std::string& ruby =
        (!sourceRubyTexts.empty() && wordIndex < sourceRubyTexts.size()) ? sourceRubyTexts[wordIndex] : emptyRuby;
    size_t segmentStart = 0;
    size_t segmentUnits = 0;

    for (size_t offset = 0; offset < word.size();) {
      const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) {
        break;
      }
      const size_t nextOffset = static_cast<size_t>(reinterpret_cast<const char*>(ptr) - word.c_str());
      ++segmentUnits;

      const size_t prospectiveUnits = current.units + segmentUnits;
      const bool safeBoundary = canEndVerticalChunkAfter(word, nextOffset, cp);
      const bool preferredBoundary = prospectiveUnits >= preferredChunkUnits && safeBoundary;
      const bool forcedBoundary = prospectiveUnits >= MAX_TATEGAKI_LAYOUT_UNITS && safeBoundary;
      if (preferredBoundary || forcedBoundary) {
        addSegment(wordIndex, segmentStart, nextOffset, sourceStyles[wordIndex],
                   segmentStart > 0 || sourceContinues[wordIndex], segmentUnits, ruby);
        finishChunk();
        segmentStart = nextOffset;
        segmentUnits = 0;
      }

      offset = nextOffset;
    }

    if (segmentStart < word.size()) {
      addSegment(wordIndex, segmentStart, word.size(), sourceStyles[wordIndex],
                 segmentStart > 0 || sourceContinues[wordIndex], segmentUnits, ruby);
    }
  }
  if (!current.words.empty()) {
    words = std::move(current.words);
    wordStyles = std::move(current.styles);
    wordContinues = std::move(current.continues);
    wordIsFocusSuffix.assign(words.size(), false);
    rubyTexts = std::move(current.rubyTexts);
    current = Chunk();
    layoutAndExtractVerticalColumns(renderer, fontId, columnHeight, processColumn, sdAdvancePrewarmed,
                                    /*includeLastColumn=*/true);
  }

  words.clear();
  wordStyles.clear();
  wordContinues.clear();
  wordIsFocusSuffix.clear();
  rubyTexts.clear();
  return true;
}

bool ParsedText::shouldUseHorizontalCjkWrapper() const {
  size_t cjkCount = 0;
  size_t otherLetterCount = 0;

  for (const auto& word : words) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
    while (const uint32_t cp = utf8NextCodepoint(&ptr)) {
      if (cp == 0x00AD || cp <= 0x20) {
        continue;
      }
      if (isCjkCodepoint(cp)) {
        ++cjkCount;
      } else {
        ++otherLetterCount;
      }
    }
  }

  return cjkCount >= 8 && cjkCount >= otherLetterCount;
}

bool ParsedText::layoutAndExtractChunkedHorizontalCjkLines(
    const GfxRenderer& renderer, const int fontId, const int pageWidth,
    const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const bool includeLastLine,
    const bool sdAdvancePrewarmed) {
  struct Chunk {
    std::vector<std::string> words;
    std::vector<EpdFontFamily::Style> styles;
    std::vector<bool> continues;
    std::vector<std::string> rubyTexts;
    size_t units = 0;
  };

  auto chunkByteCount = [](const Chunk& chunk) {
    size_t byteCount = 0;
    for (const auto& word : chunk.words) {
      byteCount += word.size();
    }
    return byteCount;
  };

  auto estimatedChunkBytes = [&chunkByteCount](const Chunk& chunk) {
    return (chunkByteCount(chunk) / 3 + chunk.words.size()) * sizeof(HorizontalCjkUnit);
  };

  std::vector<Chunk> chunks;
  Chunk current;

  auto addSegment = [&](const std::string& word, const size_t start, const size_t end, const EpdFontFamily::Style style,
                        const bool attachToPrevious, const size_t unitCount, const std::string& ruby) {
    if (start >= end) {
      return;
    }
    const bool wholeWord = start == 0 && end == word.size();
    current.words.emplace_back(word, start, end - start);
    current.styles.push_back(style);
    current.continues.push_back(!current.words.empty() && current.words.size() > 1 && attachToPrevious);
    if (!ruby.empty() || !current.rubyTexts.empty()) {
      if (current.rubyTexts.size() + 1 < current.words.size()) {
        current.rubyTexts.resize(current.words.size() - 1);
      }
      current.rubyTexts.push_back(wholeWord ? ruby : std::string());
    }
    current.units += unitCount;
  };

  auto finishChunk = [&]() {
    if (current.words.empty()) {
      return;
    }
    chunks.push_back(std::move(current));
    current = Chunk();
  };

  for (size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const std::string& word = words[wordIndex];
    const std::string emptyRuby;
    const std::string& ruby = (!rubyTexts.empty() && wordIndex < rubyTexts.size()) ? rubyTexts[wordIndex] : emptyRuby;
    size_t segmentStart = 0;
    size_t segmentUnits = 0;

    for (size_t offset = 0; offset < word.size();) {
      const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) {
        break;
      }
      const size_t nextOffset = static_cast<size_t>(reinterpret_cast<const char*>(ptr) - word.c_str());
      ++segmentUnits;

      const size_t prospectiveUnits = current.units + segmentUnits;
      const bool preferredBoundary =
          isCjkChunkBoundary(cp) && prospectiveUnits >= HORIZONTAL_CJK_LAYOUT_CHUNK_TARGET_UNITS;
      const bool forcedBoundary = prospectiveUnits >= MAX_HORIZONTAL_CJK_LAYOUT_UNITS;
      if (preferredBoundary || forcedBoundary) {
        const bool attach = segmentStart > 0 || wordContinues[wordIndex];
        addSegment(word, segmentStart, nextOffset, wordStyles[wordIndex], attach, segmentUnits, ruby);
        finishChunk();
        segmentStart = nextOffset;
        segmentUnits = 0;
      }

      offset = nextOffset;
    }

    const bool attach = segmentStart > 0 || wordContinues[wordIndex];
    addSegment(word, segmentStart, word.size(), wordStyles[wordIndex], attach, segmentUnits, ruby);
  }
  finishChunk();

  if (chunks.empty()) {
    return false;
  }

  for (const auto& chunk : chunks) {
    const size_t estimatedBytes = estimatedChunkBytes(chunk);
    if (chunk.units > MAX_HORIZONTAL_CJK_LAYOUT_UNITS || !hasHeapForHorizontalCjkLayout(estimatedBytes)) {
      return false;
    }
  }

  std::vector<std::string> remainingWords;
  std::vector<EpdFontFamily::Style> remainingStyles;
  std::vector<bool> remainingContinues;
  std::vector<bool> remainingFocusSuffixes;
  std::vector<std::string> remainingRubyTexts;

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (!remainingWords.empty()) {
      if (!chunks[i].continues.empty()) {
        chunks[i].continues.front() = true;
      }
      chunks[i].units +=
          chunkByteCount({remainingWords, remainingStyles, remainingContinues, std::vector<std::string>{}, 0}) / 3 +
          remainingWords.size();
      chunks[i].words.insert(chunks[i].words.begin(), std::make_move_iterator(remainingWords.begin()),
                             std::make_move_iterator(remainingWords.end()));
      chunks[i].styles.insert(chunks[i].styles.begin(), std::make_move_iterator(remainingStyles.begin()),
                              std::make_move_iterator(remainingStyles.end()));
      chunks[i].continues.insert(chunks[i].continues.begin(), std::make_move_iterator(remainingContinues.begin()),
                                 std::make_move_iterator(remainingContinues.end()));
      if (!remainingRubyTexts.empty() || !chunks[i].rubyTexts.empty()) {
        if (remainingRubyTexts.size() < remainingWords.size()) {
          remainingRubyTexts.resize(remainingWords.size());
        }
        if (chunks[i].rubyTexts.size() < chunks[i].words.size() - remainingWords.size()) {
          chunks[i].rubyTexts.resize(chunks[i].words.size() - remainingWords.size());
        }
        chunks[i].rubyTexts.insert(chunks[i].rubyTexts.begin(), std::make_move_iterator(remainingRubyTexts.begin()),
                                   std::make_move_iterator(remainingRubyTexts.end()));
      }
      remainingWords.clear();
      remainingStyles.clear();
      remainingContinues.clear();
      remainingFocusSuffixes.clear();
      remainingRubyTexts.clear();
    }

    BlockStyle chunkStyle = blockStyle;
    if (i > 0) {
      chunkStyle.textIndent = 0;
      chunkStyle.textIndentDefined = true;
    }

    ParsedText chunkText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, chunkStyle);
    chunkText.words = std::move(chunks[i].words);
    chunkText.wordStyles = std::move(chunks[i].styles);
    chunkText.wordContinues = std::move(chunks[i].continues);
    chunkText.wordIsFocusSuffix.assign(chunkText.words.size(), false);
    chunkText.rubyTexts = std::move(chunks[i].rubyTexts);
    const bool chunkIncludeLastLine = includeLastLine && i + 1 == chunks.size();
    if (!chunkText.layoutAndExtractHorizontalCjkLines(renderer, fontId, pageWidth, processLine, chunkIncludeLastLine,
                                                      sdAdvancePrewarmed)) {
      return false;
    }
    if (!chunkIncludeLastLine) {
      remainingWords = std::move(chunkText.words);
      remainingStyles = std::move(chunkText.wordStyles);
      remainingContinues = std::move(chunkText.wordContinues);
      remainingFocusSuffixes = std::move(chunkText.wordIsFocusSuffix);
      remainingRubyTexts = std::move(chunkText.rubyTexts);
    }
  }

  words = std::move(remainingWords);
  wordStyles = std::move(remainingStyles);
  wordContinues = std::move(remainingContinues);
  wordIsFocusSuffix = std::move(remainingFocusSuffixes);
  rubyTexts = std::move(remainingRubyTexts);
  return true;
}

bool ParsedText::layoutAndExtractHorizontalCjkLines(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                    const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                                    const bool includeLastLine, const bool sdAdvancePrewarmed) {
  std::vector<HorizontalCjkUnit> units;
  size_t byteCount = 0;
  for (const auto& word : words) {
    byteCount += word.size();
  }
  const size_t estimatedUnits = byteCount / 3 + words.size();
  const size_t estimatedBytes = estimatedUnits * sizeof(HorizontalCjkUnit);
  if (estimatedUnits > MAX_HORIZONTAL_CJK_LAYOUT_UNITS || !hasHeapForHorizontalCjkLayout(estimatedBytes)) {
    if (estimatedUnits > MAX_HORIZONTAL_CJK_LAYOUT_UNITS &&
        layoutAndExtractChunkedHorizontalCjkLines(renderer, fontId, pageWidth, processLine, includeLastLine,
                                                  sdAdvancePrewarmed)) {
      return true;
    }
    return false;
  }
  units.reserve(estimatedUnits);

  // Rotated tategaki fonts can report very narrow advances for kana/punctuation. Use a representative
  // full-width glyph as the CJK cell, with a conservative line-height fallback when the glyph is absent.
  const int representativeCjkAdvance =
      std::max(renderer.getTextAdvanceX(fontId, "\xe6\x97\xa5", EpdFontFamily::REGULAR),   // 日
               renderer.getTextAdvanceX(fontId, "\xe3\x81\x82", EpdFontFamily::REGULAR));  // あ
  const int cjkCellAdvance =
      std::max(1, representativeCjkAdvance > 0 ? representativeCjkAdvance : renderer.getLineHeight(fontId) * 9 / 10);

  // Refine the parser's coarse "words" into layout units. CJK text can usually break between
  // characters, while embedded Latin/numeric runs should stay together as a single measured unit.
  bool previousUnitWasCjk = false;
  for (uint16_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const std::string& word = words[wordIndex];
    if (!rubyTexts.empty() && wordIndex < rubyTexts.size() && !rubyTexts[wordIndex].empty()) {
      const uint32_t firstCp = firstCodepoint(word);
      const uint32_t lastCp = lastCodepoint(word);
      const bool cpIsCjk = isCjkCodepoint(firstCp);
      const bool noGapBefore = !units.empty() && (wordContinues[wordIndex] || (previousUnitWasCjk && cpIsCjk));
      const int measuredAdvance = renderer.getTextAdvanceX(fontId, word.c_str(), wordStyles[wordIndex]);
      size_t cpCount = 0;
      const auto* countPtr = reinterpret_cast<const unsigned char*>(word.c_str());
      while (utf8NextCodepoint(&countPtr)) {
        ++cpCount;
      }
      const int rubyBaseWidth =
          std::max(measuredAdvance, cjkCellAdvance * static_cast<int>(std::max<size_t>(cpCount, 1)));
      units.push_back({firstCp, lastCp, wordIndex, 0, static_cast<uint16_t>(word.size()),
                       static_cast<uint16_t>(std::max(1, rubyBaseWidth)), noGapBefore, false, cpIsCjk,
                       wordStyles[wordIndex]});
      previousUnitWasCjk = cpIsCjk;
      continue;
    }

    uint16_t offset = 0;
    bool firstUnitInWord = true;

    while (offset < word.size()) {
      const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) {
        break;
      }
      const uint16_t nextOffset = static_cast<uint16_t>(reinterpret_cast<const char*>(ptr) - word.c_str());
      const bool cpIsCjk = isCjkCodepoint(cp);
      const bool noGapBefore = !units.empty() && (!firstUnitInWord || (firstUnitInWord && wordContinues[wordIndex]) ||
                                                  (previousUnitWasCjk && cpIsCjk));
      const bool noBreakBefore = !units.empty() && firstUnitInWord && wordContinues[wordIndex] && !cpIsCjk;

      if (!cpIsCjk) {
        uint16_t runEnd = nextOffset;
        uint32_t lastCpInRun = cp;
        while (runEnd < word.size()) {
          const auto* lookaheadPtr = reinterpret_cast<const unsigned char*>(word.c_str() + runEnd);
          const uint32_t lookaheadCp = utf8NextCodepoint(&lookaheadPtr);
          if (lookaheadCp == 0 || isCjkCodepoint(lookaheadCp)) {
            break;
          }
          lastCpInRun = lookaheadCp;
          runEnd = static_cast<uint16_t>(reinterpret_cast<const char*>(lookaheadPtr) - word.c_str());
        }
        const std::string run = word.substr(offset, runEnd - offset);
        units.push_back({cp, lastCpInRun, wordIndex, offset, runEnd,
                         measureRunWidth(renderer, fontId, run, wordStyles[wordIndex]), noGapBefore, noBreakBefore,
                         false, wordStyles[wordIndex]});
        previousUnitWasCjk = false;
        offset = runEnd;
        firstUnitInWord = false;
        continue;
      }

      char cpText[5] = {};
      const size_t cpBytes = nextOffset - offset;
      for (size_t i = 0; i < cpBytes && i < 4; ++i) {
        cpText[i] = word[offset + i];
      }
      const int measuredAdvance = renderer.getTextAdvanceX(fontId, cpText, wordStyles[wordIndex]);
      const int unitWidth = isCjkClosingPunctuation(cp) && measuredAdvance > 0
                                ? measuredAdvance
                                : std::max(cjkCellAdvance, measuredAdvance);
      units.push_back({cp, cp, wordIndex, offset, nextOffset, static_cast<uint16_t>(std::max(1, unitWidth)),
                       noGapBefore, noBreakBefore, true, wordStyles[wordIndex]});
      previousUnitWasCjk = true;
      offset = nextOffset;
      firstUnitInWord = false;
    }
  }
  if (units.empty()) {
    words.clear();
    wordStyles.clear();
    wordContinues.clear();
    wordIsFocusSuffix.clear();
    rubyTexts.clear();
    return true;
  }

  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  auto canBreakAfter = [&units](const size_t unitIndex) {
    if (unitIndex + 1 >= units.size()) {
      return true;
    }
    if (units[unitIndex + 1].noBreakBefore) {
      return false;
    }
    return !isCjkNoLineEnd(units[unitIndex].lastCp) && !isCjkNoLineStart(units[unitIndex + 1].firstCp);
  };

  auto gapBefore = [&renderer, fontId](const HorizontalCjkUnit& previous, const HorizontalCjkUnit& current) {
    if (current.noGapBefore) {
      if (previous.isCjk || current.isCjk) {
        return 0;
      }
      return renderer.getKerning(fontId, previous.lastCp, current.firstCp, previous.style);
    }
    return renderer.getSpaceAdvance(fontId, previous.lastCp, current.firstCp, previous.style);
  };

  auto leadingOpeningHangWidth = [&units, firstLineIndent](const bool isFirstLine, const size_t start) {
    if (!isFirstLine || start != 0 || firstLineIndent <= 0 || units.empty()) {
      return 0;
    }
    const HorizontalCjkUnit& unit = units.front();
    if (!unit.isCjk || !isCjkOpeningPunctuation(unit.firstCp)) {
      return 0;
    }
    return std::min<int>(firstLineIndent, unit.width);
  };

  auto emitLine = [&](const size_t start, const size_t end, const int lineWidth, const bool isFirstLine) {
    if (start >= end) {
      return;
    }

    const int hangingOpeningWidth = leadingOpeningHangWidth(isFirstLine, start);
    const int effectivePageWidth = pageWidth - (isFirstLine ? firstLineIndent : 0) + hangingOpeningWidth;
    int lineOffset = (isFirstLine ? firstLineIndent : 0) - hangingOpeningWidth;
    if (blockStyle.alignment == CssTextAlign::Right) {
      lineOffset += effectivePageWidth - lineWidth;
    } else if (blockStyle.alignment == CssTextAlign::Center) {
      lineOffset += (effectivePageWidth - lineWidth) / 2;
    }

    std::vector<std::string> lineWords;
    std::vector<int16_t> lineXPos;
    std::vector<uint16_t> lineWordWidths;
    std::vector<EpdFontFamily::Style> lineWordStyles;
    std::vector<std::string> lineRubyTexts;
    const bool hasRubyInSource = !rubyTexts.empty();
    lineWords.reserve(end - start);
    lineXPos.reserve(end - start);
    lineWordWidths.reserve(end - start);
    lineWordStyles.reserve(end - start);
    if (hasRubyInSource) {
      lineRubyTexts.reserve(end - start);
    }

    // Build the TextBlock's parallel arrays for this single rendered line. CJK units stay
    // individually positioned so rotated-font advance quirks cannot collapse neighboring glyphs.
    int xpos = lineOffset;
    for (size_t i = start; i < end; ++i) {
      if (i > start) {
        xpos += gapBefore(units[i - 1], units[i]);
      }

      const HorizontalCjkUnit& unit = units[i];
      const std::string& source = words[unit.wordIndex];
      const bool canMerge = !lineWords.empty() && unit.noGapBefore && !unit.isCjk && !units[i - 1].isCjk &&
                            lineWordStyles.back() == unit.style;
      if (canMerge) {
        lineWords.back().append(source, unit.startByte, unit.endByte - unit.startByte);
        lineWordWidths.back() =
            static_cast<uint16_t>(std::min<int>(UINT16_MAX, xpos - static_cast<int>(lineXPos.back()) + unit.width));
      } else {
        lineWords.emplace_back(source, unit.startByte, unit.endByte - unit.startByte);
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWordWidths.push_back(unit.width);
        lineWordStyles.push_back(unit.style);
        if (hasRubyInSource) {
          const bool wholeSourceWord = unit.startByte == 0 && unit.endByte == source.size();
          lineRubyTexts.push_back(wholeSourceWord && unit.wordIndex < rubyTexts.size() ? rubyTexts[unit.wordIndex]
                                                                                       : std::string());
        }
      }
      xpos += unit.width;
    }

    bool hasUnderline = false;
    for (auto style : lineWordStyles) {
      if ((style & EpdFontFamily::UNDERLINE) != 0) {
        hasUnderline = true;
        break;
      }
    }

    processLine(std::make_shared<TextBlock>(
        std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), std::vector<uint8_t>{},
        std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts), std::vector<int16_t>{}, false,
        std::vector<uint16_t>{}, 0, hasUnderline ? std::move(lineWordWidths) : std::vector<uint16_t>{}));
  };

  auto retainFromUnit = [&](const size_t unitIndex) {
    if (unitIndex >= units.size()) {
      words.clear();
      wordStyles.clear();
      wordContinues.clear();
      wordIsFocusSuffix.clear();
      rubyTexts.clear();
      return;
    }

    const HorizontalCjkUnit& firstRemaining = units[unitIndex];
    const size_t wordIndex = firstRemaining.wordIndex;
    const size_t byteOffset = firstRemaining.startByte;
    words.erase(words.begin(), words.begin() + wordIndex);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + wordIndex);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + wordIndex);
    wordIsFocusSuffix.erase(wordIsFocusSuffix.begin(), wordIsFocusSuffix.begin() + wordIndex);
    if (!rubyTexts.empty()) {
      const size_t rubyEraseCount = std::min(wordIndex, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rubyEraseCount);
    }
    if (byteOffset > 0 && !words.empty()) {
      words[0].erase(0, byteOffset);
      if (!rubyTexts.empty()) {
        rubyTexts[0].clear();
      }
    }
    if (!wordContinues.empty()) {
      wordContinues[0] = false;
    }
    if (!wordIsFocusSuffix.empty()) {
      wordIsFocusSuffix[0] = false;
    }
  };

  struct CjkLineRange {
    size_t start;
    size_t end;
    int width;
    bool isFirstLine;
  };
  std::vector<CjkLineRange> lineRanges;

  size_t lineStart = 0;
  bool isFirstLine = true;
  while (lineStart < units.size()) {
    const int hangingOpeningWidth = leadingOpeningHangWidth(isFirstLine, lineStart);
    const int effectivePageWidth = pageWidth - (isFirstLine ? firstLineIndent : 0) + hangingOpeningWidth;
    int lineWidth = 0;
    size_t bestBreak = lineStart;
    int widthAtBestBreak = 0;
    size_t i = lineStart;

    // Greedily pack units until the next one would exceed the page width, remembering the
    // latest legal kinsoku-aware break point as the preferred end of the line.
    for (; i < units.size(); ++i) {
      int candidateWidth = lineWidth + units[i].width;
      if (i > lineStart) {
        candidateWidth += gapBefore(units[i - 1], units[i]);
      }

      if (candidateWidth > effectivePageWidth && i > lineStart) {
        break;
      }

      lineWidth = candidateWidth;
      if (canBreakAfter(i)) {
        bestBreak = i + 1;
        widthAtBestBreak = lineWidth;
      }
    }

    if (i >= units.size()) {
      lineRanges.push_back({lineStart, units.size(), lineWidth, isFirstLine});
      break;
    }

    size_t lineEnd = bestBreak > lineStart ? bestBreak : i;
    int emittedWidth = bestBreak > lineStart ? widthAtBestBreak : lineWidth;
    if (lineEnd <= lineStart) {
      lineEnd = lineStart + 1;
      emittedWidth = units[lineStart].width;
    }

    lineRanges.push_back({lineStart, lineEnd, emittedWidth, isFirstLine});
    lineStart = lineEnd;
    isFirstLine = false;
  }
  const size_t lineCount = includeLastLine ? lineRanges.size() : lineRanges.size() - 1;
  for (size_t i = 0; i < lineCount; ++i) {
    emitLine(lineRanges[i].start, lineRanges[i].end, lineRanges[i].width, lineRanges[i].isFirstLine);
  }

  if (includeLastLine) {
    words.clear();
    wordStyles.clear();
    wordContinues.clear();
    wordIsFocusSuffix.clear();
    rubyTexts.clear();
  } else if (lineCount > 0) {
    retainFromUnit(lineRanges[lineCount].start);
  }
  return true;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(true);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (extraParagraphSpacing || words.empty()) {
    return;
  }

  if (removeLeadingIndentBeforeCjkOpeningPunctuation(words.front())) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback EmSpace
    // The actual indent positioning is handled in extractLine()
  } else if (isNaturalAlign && !startsWithParagraphIndentSpace(words.front()) &&
             !startsWithCjkOpeningPunctuationInCjkText(words.front())) {
    // No CSS text-indent defined - use EmSpace fallback for visual indent.
    // Leading CJK opening punctuation hangs in this space, so don't insert an extra one.
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  const int firstLineIndent = resolveFirstLineIndent(true);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && !continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && continuesVec[currentIndex]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  if (!rubyTexts.empty()) {
    if (rubyTexts.size() < words.size() - 1) {
      rubyTexts.resize(words.size() - 1);
    }
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }
  // The hyphen remainder is not a focus suffix - it starts fresh on the next line.
  wordIsFocusSuffix.insert(wordIsFocusSuffix.begin() + wordIndex + 1, false);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0);

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                                   firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (lineWords[wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? spareSpace / static_cast<int>(actualGapCount)
                               : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedFocusSuffixScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedFocusSuffixScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusSuffixScratch.push_back(wordIsFocusSuffix[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        reorderedGapCount++;
        reorderedNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                         reorderedStylesScratch[wordIdx - 1]);
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " ") {
          reorderedGapCount++;
        }
        reorderedNaturalGaps +=
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                firstCodepoint(reorderedWordsScratch[wordIdx]), reorderedStylesScratch[wordIdx - 1]);
      }
    }

    const int reorderedSpare = effectivePageWidth - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra =
        (effectiveAlignment == CssTextAlign::Justify && !isLastLine && reorderedGapCount >= 1)
            ? reorderedSpare / static_cast<int>(reorderedGapCount)
            : 0;

    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos < 0 ? 0 : xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance =
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                firstCodepoint(reorderedWordsScratch[wordIdx + 1]), reorderedStylesScratch[wordIdx]);
        if (reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedJustifyExtra;
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        int gap = renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                           firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                           reorderedStylesScratch[wordIdx]);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedJustifyExtra;
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      auto xpos = static_cast<int>(effectivePageWidth);
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos < 0 ? 0 : xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance = renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                            firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          if (lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos -= advance;
        } else {
          int gap = 0;
          if (wordIdx + 1 < lineWordCount) {
            gap = renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                           firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      auto xpos = static_cast<int16_t>(firstLineIndent);
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        lineXPos.push_back(static_cast<int16_t>(xpos < 0 ? 0 : xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx];
          advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                         firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          if (lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos += advance;
        } else {
          int gap = 0;
          if (wordIdx + 1 < lineWordCount) {
            gap = renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                           firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos += wordWidths[lastBreakAt + wordIdx] + gap;
        }
      }
    }
  }

  std::vector<uint16_t> lineWordWidths;
  if (willReorder) {
    lineWordWidths.assign(reorderedWidthsScratch.begin(), reorderedWidthsScratch.end());
  } else {
    lineWordWidths.assign(wordWidths.begin() + lastBreakAt, wordWidths.begin() + lineBreak);
  }

  std::vector<std::string> lineRubyTexts;
  if (!rubyTexts.empty()) {
    lineRubyTexts.reserve(lineWordCount);
    bool hasRuby = false;
    if (willReorder) {
      for (const uint16_t src : visualOrderScratch) {
        std::string ruby = lastBreakAt + src < rubyTexts.size() ? rubyTexts[lastBreakAt + src] : std::string();
        hasRuby = hasRuby || !ruby.empty();
        lineRubyTexts.push_back(std::move(ruby));
      }
    } else {
      lineRubyTexts = sliceRubyTexts(rubyTexts, lastBreakAt, lineBreak);
      hasRuby = !lineRubyTexts.empty();
    }
    if (!hasRuby) {
      lineRubyTexts.clear();
    }
  }

  const auto isFocusSuffixAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusSuffixScratch[idx] : wordIsFocusSuffix[lastBreakAt + idx];
  };

  bool lineHasUnderline = false;
  for (auto style : lineWordStyles) {
    if ((style & EpdFontFamily::UNDERLINE) != 0) {
      lineHasUnderline = true;
      break;
    }
  }

  // Fast path: when no word on this line was split for focus reading, skip the merge work
  // entirely and pass empty boundary/suffixX vectors. TextBlock pays zero per-word RAM cost
  // for these annotations when the vectors are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i)) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    processLine(std::make_shared<TextBlock>(
        std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), std::vector<uint8_t>{},
        std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts), std::vector<int16_t>{}, false,
        std::vector<uint16_t>{}, 0, lineHasUnderline ? std::move(lineWordWidths) : std::vector<uint16_t>{}));
    return;
  }

  // Slow path: merge focus suffix tokens back into their preceding word entry so each
  // original word occupies one TextBlock slot. Splits are recorded as per-word annotations
  // applied at render time, cutting the token count significantly when the feature is active.
  std::vector<std::string> outWords;
  std::vector<int16_t> outXPos;
  std::vector<uint16_t> outWidths;
  std::vector<EpdFontFamily::Style> outStyles;
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  std::vector<std::string> outRubyTexts;
  const bool lineHasRuby = !lineRubyTexts.empty();
  outWords.reserve(lineWordCount);
  outXPos.reserve(lineWordCount);
  outWidths.reserve(lineWordCount);
  outStyles.reserve(lineWordCount);
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);
  if (lineHasRuby) {
    outRubyTexts.reserve(lineWordCount);
  }

  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i) && !outWords.empty()) {
      // Focus suffix: merge string into the preceding bold-prefix entry.
      outWords.back() += lineWords[i];
      outWidths.back() =
          static_cast<uint16_t>(std::min<int>(UINT16_MAX, lineXPos[i] - outXPos.back() + lineWordWidths[i]));
    } else {
      // Normal word: check for a following focus suffix to record the byte boundary.
      uint8_t boundary = 0;
      uint16_t suffixX = 0;
      if (i + 1 < lineWordCount && isFocusSuffixAt(i + 1)) {
        boundary = static_cast<uint8_t>(std::min(lineWords[i].size(), size_t{255}));
        // Suffix x offset = layout-time advance of the bold prefix, already known from xpos table.
        const int suffixDelta = static_cast<int>(lineXPos[i + 1]) - static_cast<int>(lineXPos[i]);
        suffixX = static_cast<uint16_t>(suffixDelta > 0 ? suffixDelta : 0);
      }
      outWords.push_back(std::move(lineWords[i]));
      outXPos.push_back(lineXPos[i]);
      outWidths.push_back(lineWordWidths[i]);
      // For focus entries with a suffix, strip BOLD from the stored style.
      // Render re-applies it to the prefix portion only, via the boundary field.
      const EpdFontFamily::Style storedStyle =
          boundary > 0 ? static_cast<EpdFontFamily::Style>(lineWordStyles[i] & ~EpdFontFamily::BOLD)
                       : lineWordStyles[i];
      outStyles.push_back(storedStyle);
      outBoundaries.push_back(boundary);
      outSuffixX.push_back(suffixX);
      if (lineHasRuby) {
        outRubyTexts.push_back(i < lineRubyTexts.size() ? std::move(lineRubyTexts[i]) : std::string());
      }
    }
  }

  processLine(std::make_shared<TextBlock>(
      std::move(outWords), std::move(outXPos), std::move(outStyles), std::move(outBoundaries), std::move(outSuffixX),
      blockStyle, std::move(outRubyTexts), std::vector<int16_t>{}, false, std::vector<uint16_t>{}, 0,
      lineHasUnderline ? std::move(outWidths) : std::vector<uint16_t>{}));
}
