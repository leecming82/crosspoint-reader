#include "ParsedText.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Utf8.h>

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
constexpr size_t MIN_FREE_HEAP_FOR_CJK_LAYOUT = 48 * 1024;
constexpr size_t MIN_CJK_LAYOUT_ALLOC_MARGIN = 8 * 1024;
constexpr size_t MAX_CJK_LAYOUT_UNITS = 1024;

struct CjkUnit {
  size_t wordIndex;
  size_t startByte;
  size_t endByte;
  uint32_t firstCp;
  uint32_t lastCp;
  uint16_t width;
  bool noGapBefore;
  bool noBreakBefore;
  bool isCjk;
  EpdFontFamily::Style style;
};

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

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

bool isCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||  // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK unified ideographs
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||  // Fullwidth forms
         (cp >= 0x20000 && cp <= 0x2FA1F);  // CJK extensions B-F + compatibility supplement
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

bool isCjkNoLineEnd(const uint32_t cp) {
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

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Reserve upfront so the joined text allocates exactly once. Without this,
    // paragraphs with many words trigger a chain of vector-like reallocations
    // inside std::string during layout — visible in prewarm timings for SD fonts.
    size_t totalSize = hyphenationEnabled ? 1 : 0;
    if (!words.empty()) totalSize += words.size() - 1;  // inter-word spaces
    for (const auto& w : words) totalSize += w.size();
    std::string allText;
    allText.reserve(totalSize);
    for (size_t i = 0; i < words.size(); i++) {
      if (i > 0) allText += ' ';
      allText += words[i];
    }
    if (hyphenationEnabled) allText += '-';

    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    renderer.ensureSdCardFontReady(fontId, allText.c_str(), styleMask);
  }

  const int pageWidth = viewportWidth;
  if (includeLastLine && shouldUseCjkWrapper()) {
    if (layoutAndExtractCjkLines(renderer, fontId, pageWidth, processLine)) {
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
  }
}

bool ParsedText::shouldUseCjkWrapper() const {
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

bool ParsedText::layoutAndExtractCjkLines(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                          const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  std::vector<CjkUnit> units;
  size_t byteCount = 0;
  for (const auto& word : words) {
    byteCount += word.size();
  }
  const size_t estimatedUnits = byteCount / 3 + words.size();
  const size_t estimatedBytes = estimatedUnits * sizeof(CjkUnit);
  if (estimatedUnits > MAX_CJK_LAYOUT_UNITS || ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CJK_LAYOUT ||
      ESP.getMaxAllocHeap() < estimatedBytes + MIN_CJK_LAYOUT_ALLOC_MARGIN) {
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
  for (size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
    const std::string& word = words[wordIndex];
    size_t offset = 0;
    bool firstUnitInWord = true;

    while (offset < word.size()) {
      const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (cp == 0) {
        break;
      }
      const size_t nextOffset = static_cast<size_t>(reinterpret_cast<const char*>(ptr) - word.c_str());
      const bool cpIsCjk = isCjkCodepoint(cp);
      const bool noGapBefore = !units.empty() && (!firstUnitInWord || (firstUnitInWord && wordContinues[wordIndex]) ||
                                                  (previousUnitWasCjk && cpIsCjk));
      const bool noBreakBefore = !units.empty() && firstUnitInWord && wordContinues[wordIndex];

      if (!cpIsCjk) {
        size_t runEnd = nextOffset;
        uint32_t lastCpInRun = cp;
        while (runEnd < word.size()) {
          const auto* lookaheadPtr = reinterpret_cast<const unsigned char*>(word.c_str() + runEnd);
          const uint32_t lookaheadCp = utf8NextCodepoint(&lookaheadPtr);
          if (lookaheadCp == 0 || isCjkCodepoint(lookaheadCp)) {
            break;
          }
          lastCpInRun = lookaheadCp;
          runEnd = static_cast<size_t>(reinterpret_cast<const char*>(lookaheadPtr) - word.c_str());
        }
        const std::string run = word.substr(offset, runEnd - offset);
        units.push_back({wordIndex, offset, runEnd, cp, lastCpInRun,
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
      units.push_back({wordIndex, offset, nextOffset, cp, cp,
                       static_cast<uint16_t>(
                           std::max(cjkCellAdvance, renderer.getTextAdvanceX(fontId, cpText, wordStyles[wordIndex]))),
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

  auto gapBefore = [&renderer, fontId](const CjkUnit& previous, const CjkUnit& current) {
    if (current.noGapBefore) {
      if (previous.isCjk || current.isCjk) {
        return 0;
      }
      return renderer.getKerning(fontId, previous.lastCp, current.firstCp, previous.style);
    }
    return renderer.getSpaceAdvance(fontId, previous.lastCp, current.firstCp, previous.style);
  };

  auto emitLine = [&](const size_t start, const size_t end, const int lineWidth, const bool isFirstLine) {
    if (start >= end) {
      return;
    }

    const int effectivePageWidth = pageWidth - (isFirstLine ? firstLineIndent : 0);
    int lineOffset = isFirstLine ? firstLineIndent : 0;
    if (blockStyle.alignment == CssTextAlign::Right) {
      lineOffset += effectivePageWidth - lineWidth;
    } else if (blockStyle.alignment == CssTextAlign::Center) {
      lineOffset += (effectivePageWidth - lineWidth) / 2;
    }

    std::vector<std::string> lineWords;
    std::vector<int16_t> lineXPos;
    std::vector<EpdFontFamily::Style> lineWordStyles;
    lineWords.reserve(end - start);
    lineXPos.reserve(end - start);
    lineWordStyles.reserve(end - start);

    // Build the TextBlock's parallel arrays for this single rendered line. CJK units stay
    // individually positioned so rotated-font advance quirks cannot collapse neighboring glyphs.
    int xpos = lineOffset;
    for (size_t i = start; i < end; ++i) {
      if (i > start) {
        xpos += gapBefore(units[i - 1], units[i]);
      }

      const CjkUnit& unit = units[i];
      const std::string& source = words[unit.wordIndex];
      const bool canMerge = !lineWords.empty() && unit.noGapBefore && !unit.isCjk && !units[i - 1].isCjk &&
                            lineWordStyles.back() == unit.style;
      if (canMerge) {
        lineWords.back().append(source, unit.startByte, unit.endByte - unit.startByte);
      } else {
        lineWords.emplace_back(source, unit.startByte, unit.endByte - unit.startByte);
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWordStyles.push_back(unit.style);
      }
      xpos += unit.width;
    }

    processLine(
        std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
  };

  size_t lineStart = 0;
  bool isFirstLine = true;
  while (lineStart < units.size()) {
    const int effectivePageWidth = pageWidth - (isFirstLine ? firstLineIndent : 0);
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

      if (candidateWidth > effectivePageWidth) {
        ++i;
        break;
      }
    }

    if (i >= units.size()) {
      emitLine(lineStart, units.size(), lineWidth, isFirstLine);
      break;
    }

    size_t lineEnd = bestBreak > lineStart ? bestBreak : i;
    int emittedWidth = bestBreak > lineStart ? widthAtBestBreak : lineWidth;
    if (lineEnd <= lineStart) {
      lineEnd = lineStart + 1;
      emittedWidth = units[lineStart].width;
    }

    emitLine(lineStart, lineEnd, emittedWidth, isFirstLine);
    lineStart = lineEnd;
    isFirstLine = false;
  }

  words.clear();
  wordStyles.clear();
  wordContinues.clear();
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

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

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

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback EmSpace
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use EmSpace fallback for visual indent
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

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

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

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
      totalNaturalGaps +=
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                                   firstCodepoint(words[lastBreakAt + wordIdx]), wordStyles[lastBreakAt + wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps +=
          renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                              firstCodepoint(words[lastBreakAt + wordIdx]), wordStyles[lastBreakAt + wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? spareSpace / static_cast<int>(actualGapCount)
                               : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance +=
          renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                              firstCodepoint(words[lastBreakAt + wordIdx + 1]), wordStyles[lastBreakAt + wordIdx]);
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                       firstCodepoint(words[lastBreakAt + wordIdx + 1]),
                                       wordStyles[lastBreakAt + wordIdx]);
      }
      if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        gap += justifyExtra;
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
