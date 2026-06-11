#pragma once

#ifdef CROSSPOINT_BOARD_MURPHY_M4

#include <GfxRenderer.h>
#include <TtfRuntimeFont.h>

#ifdef CROSSPOINT_TTF_READER_OPENFONTRENDER
#define CROSSPOINT_TTF_USE_OPENFONTRENDER 1
#include <OpenFontRender.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TtfReaderMetrics final : public ReaderFontMetricsProvider {
 public:
  static constexpr int INVALID_FONT_ID = 0;

  bool ensureLoadedFromSettings();
  bool ensureLoaded(const char* path, uint8_t pixelSize, uint32_t expectedFileSize = 0);
  void unload();
  bool isLoaded() const { return loaded_; }
  int fontId() const { return loaded_ ? fontId_ : INVALID_FONT_ID; }
  const char* path() const { return path_.c_str(); }
  uint8_t pixelSize() const { return pixelSize_; }
  uint32_t fileSize() const { return fileSize_; }
  uint32_t identityHash() const { return identityHash_; }
  bool flushGlyphSidecarCache() const;
#ifdef CROSSPOINT_TTF_PROBE
  bool probeLoadFromPath(const char* path, uint8_t pixelSize, uint32_t expectedFileSize = 0) {
    return loadFromPath(path, pixelSize, expectedFileSize);
  }
  void probeRasterText(const char* text) const;
#endif

  bool handlesFontId(int fontId) const override;
  void prewarmText(int fontId, const char* utf8Text, uint8_t styleMask) const override;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style) const override;
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const override;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const override;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const override;
  bool canRenderText(int fontId, const char* text, EpdFontFamily::Style style) const override;
  uint32_t getVerticalSubstitution(int fontId, uint32_t cp, EpdFontFamily::Style style) const override;
  int getFontAscenderSize(int fontId) const override;
  int getLineHeight(int fontId) const override;
  void drawText(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, bool black,
                EpdFontFamily::Style style) const override;
  void drawTextRotated90CW(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, bool black,
                           EpdFontFamily::Style style) const override;

  struct PsramFreeDeleter {
    void operator()(uint8_t* ptr) const;
  };

  using PsramBuffer = std::unique_ptr<uint8_t, PsramFreeDeleter>;
  struct OwnedTable {
    PsramBuffer data;
    uint32_t length = 0;

    ttf::TableView view() const { return {data.get(), length}; }
    void reset() {
      data.reset();
      length = 0;
    }
  };

  struct TableCache {
    OwnedTable cmap;
    OwnedTable head;
    OwnedTable hhea;
    OwnedTable hmtx;
    OwnedTable loca;
    OwnedTable maxp;

    void reset();
    ttf::RuntimeTableSet views() const;
  };

  struct CachedGlyph {
    uint32_t codepoint = 0;
    uint16_t glyphId = 0;
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    int advancePx = 0;
    PsramBuffer bitmap;
    size_t bitmapBytes = 0;
    uint32_t lastUsed = 0;
  };

 private:
  bool loadFromPath(const char* path, uint8_t pixelSize, uint32_t expectedFileSize);
  int advanceForCodepoint(uint32_t cp, EpdFontFamily::Style style) const;
  int lineHeightPx() const;
  int ascenderPx() const;
  const CachedGlyph* glyphForCodepoint(uint32_t cp, EpdFontFamily::Style style) const;
  const CachedGlyph* rasterizeAndCacheGlyph(uint32_t cp, EpdFontFamily::Style style) const;
  const CachedGlyph* rasterizeAndCacheGlyphWithOpenFontRender(uint32_t cp, EpdFontFamily::Style style,
                                                              const ttf::GlyphMetrics& metrics) const;
  const CachedGlyph* rasterizeAndCacheGlyphWithCustomRasterizer(uint32_t cp, EpdFontFamily::Style style,
                                                                const ttf::GlyphMetrics& metrics) const;
  bool readGlyphSlice(const ttf::GlyphMetrics& glyph, PsramBuffer& outData, uint32_t& outLength) const;
  bool reserveGlyphCacheBytes(size_t incomingBytes) const;
  std::string glyphSidecarPath() const;
  bool loadGlyphSidecarCache() const;
  bool saveGlyphSidecarCache() const;
  bool maybeSaveGlyphSidecarCache() const;
  void markGlyphSidecarDirty(size_t bytes) const;
  void clearGlyphCache() const;
  void logRenderStats(const char* label) const;
#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  void logOpenFontRenderMetricSamples() const;
#endif
  static uint32_t computeIdentityHash(const char* path, uint8_t pixelSize, uint32_t fileSize);

  TableCache tables_;
  ttf::TtfRuntimeFont font_;
  std::string path_;
  uint8_t pixelSize_ = 0;
  uint32_t fileSize_ = 0;
  uint32_t identityHash_ = 0;
  uint32_t glyfTableOffset_ = 0;
  uint32_t glyfTableLength_ = 0;
  int fontId_ = INVALID_FONT_ID;
  bool loaded_ = false;
#ifdef CROSSPOINT_TTF_USE_OPENFONTRENDER
  mutable OpenFontRender openFontRender_;
  bool openFontRenderLoaded_ = false;
#endif
  mutable std::vector<CachedGlyph> glyphCache_;
  mutable size_t glyphCacheBytes_ = 0;
  mutable uint32_t glyphUseClock_ = 0;
  mutable uint32_t cacheHits_ = 0;
  mutable uint32_t cacheMisses_ = 0;
  mutable uint32_t rasterOk_ = 0;
  mutable uint32_t rasterFailed_ = 0;
  mutable uint32_t compoundGlyphs_ = 0;
  mutable uint32_t missingGlyphs_ = 0;
  mutable uint32_t renderedGlyphs_ = 0;
  mutable uint32_t cacheResets_ = 0;
  mutable uint32_t cacheEvictions_ = 0;
  mutable bool glyphSidecarDirty_ = false;
  mutable uint32_t glyphSidecarDirtyGlyphs_ = 0;
  mutable size_t glyphSidecarDirtyBytes_ = 0;
  mutable unsigned long lastGlyphSidecarSaveMs_ = 0;
  mutable uint64_t rasterTimeUs_ = 0;
  mutable uint64_t downsampleTimeUs_ = 0;
  mutable uint32_t lastLoggedRasterOk_ = 0;
  mutable uint32_t lastLoggedCacheMisses_ = 0;
  mutable uint32_t lastLoggedRenderedGlyphs_ = 0;
  mutable uint64_t lastLoggedRasterTimeUs_ = 0;
  mutable unsigned long lastStatsLogMs_ = 0;
};

extern TtfReaderMetrics TTF_READER_METRICS;

#endif
