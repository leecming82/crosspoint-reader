#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous (no space before it)
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  std::vector<std::string> rubyTexts;   // Lazy sidecar parallel to words; empty means no ruby in this block.
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;

  void applyParagraphIndent(bool useCjkWrapper);
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
  bool shouldUseCjkWrapper() const;
  bool layoutAndExtractCjkLines(const GfxRenderer& renderer, int fontId, int pageWidth,
                                const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                bool includeLastLine = true, bool sdAdvancePrewarmed = false);
  bool layoutAndExtractChunkedYokogakiCjkLines(const GfxRenderer& renderer, int fontId, int pageWidth,
                                               const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                               bool includeLastLine = true, bool sdAdvancePrewarmed = false);
  bool layoutAndExtractChunkedTategakiColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                                              const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                                              bool sdAdvancePrewarmed = false);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool tateChuYoko = false);
  void addRubyWord(std::string word, std::string ruby, EpdFontFamily::Style fontStyle, bool underline = false,
                   bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, bool sdAdvancePrewarmed = false);
  void layoutAndExtractVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                                       bool sdAdvancePrewarmed = false, bool includeLastColumn = true);
};

// Returns true for CJK unified ideographs (kanji) only.
// Excludes hiragana, katakana, punctuation, and fullwidth forms.
inline bool isKanjiCodepoint(const uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs (core)
         (cp >= 0x20000 && cp <= 0x2FA1F);  // CJK Extensions B-F + compat supp
}
