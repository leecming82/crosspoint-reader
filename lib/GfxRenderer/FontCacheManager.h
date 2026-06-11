#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class ReaderFontPrewarmProvider {
 public:
  virtual ~ReaderFontPrewarmProvider() = default;
  virtual bool handlesFontId(int fontId) const = 0;
  virtual void prewarmText(int fontId, const char* utf8Text, uint8_t styleMask) const = 0;
};

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);
  void setReaderFontPrewarmProvider(ReaderFontPrewarmProvider* provider);

  void clearCache();
  void clearPersistentCache();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
  ReaderFontPrewarmProvider* readerFontPrewarmProvider_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  struct ScanFontRecord {
    std::string text;
    uint32_t styleCounts[4] = {};
  };
  ScanMode scanMode_ = ScanMode::None;
  std::map<int, ScanFontRecord> scanFonts_;
};
