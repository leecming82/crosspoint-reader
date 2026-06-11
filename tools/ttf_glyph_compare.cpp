#include <TtfCustomRasterizer.h>
#include <TtfProbe.h>
#include <TtfRuntimeFont.h>
#include <TtfStb.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kBitmapScratchBytes = 512 * 1024;

uint16_t readU16BE(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint16_t readU16LE(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

int16_t readI16LE(const uint8_t* p) { return static_cast<int16_t>(readU16LE(p)); }

uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

ttf::TableView tableView(const std::vector<uint8_t>& bytes, const uint32_t tag) {
  if (bytes.size() < 12) return {};
  const uint16_t tableCount = readU16BE(bytes.data() + 4);
  const size_t recordsSize = 12 + static_cast<size_t>(tableCount) * 16;
  if (recordsSize > bytes.size()) return {};

  for (uint16_t i = 0; i < tableCount; ++i) {
    const uint8_t* record = bytes.data() + 12 + static_cast<size_t>(i) * 16;
    if (readU32BE(record) != tag) continue;
    const uint32_t offset = readU32BE(record + 8);
    const uint32_t length = readU32BE(record + 12);
    if (offset > bytes.size() || length > bytes.size() - offset) return {};
    return {bytes.data() + offset, length};
  }

  return {};
}

bool loadFile(const char* path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

uint32_t parseCodepoint(const char* text) {
  if (!text || *text == '\0') return 0;
  char* end = nullptr;
  unsigned long value = 0;
  if (text[0] == 'U' && text[1] == '+') {
    value = std::strtoul(text + 2, &end, 16);
  } else {
    value = std::strtoul(text, &end, 0);
  }
  return end && *end == '\0' && value <= 0x10FFFFul ? static_cast<uint32_t>(value) : 0;
}

std::string cpLabel(const uint32_t cp) {
  std::ostringstream out;
  out << "U+" << std::uppercase << std::hex << std::setw(cp > 0xFFFF ? 6 : 4) << std::setfill('0') << cp;
  return out.str();
}

struct BitmapStats {
  size_t inkPixels = 0;
  size_t weakPixels = 0;
  size_t strongPixels = 0;
  uint64_t coverageSum = 0;
};

struct GrayBitmap {
  int width = 0;
  int height = 0;
  int xOffset = 0;
  int yOffset = 0;
  std::vector<uint8_t> pixels;
};

struct PngGlyphRow {
  uint32_t codepoint = 0;
  GrayBitmap cpfont;
  GrayBitmap custom1Bit;
  GrayBitmap custom2xEvenOdd;
  GrayBitmap custom2xNonZero;
  GrayBitmap custom3xEvenOdd;
  GrayBitmap stb;
};

struct CpGlyph {
  bool present = false;
  uint8_t width = 0;
  uint8_t height = 0;
  uint16_t advanceX = 0;
  int16_t left = 0;
  int16_t top = 0;
  uint16_t dataLength = 0;
  uint32_t dataOffset = 0;
  const uint8_t* bitmap = nullptr;
};

class CpFont {
 public:
  bool load(const char* path, const char** error) {
    path_ = path ? path : "";
    if (!loadFile(path, bytes_)) {
      setError(error, "open failed");
      return false;
    }
    if (bytes_.size() < 64) {
      setError(error, "file too small");
      return false;
    }
    if (std::memcmp(bytes_.data(), "CPFONT\0\0", 8) != 0) {
      setError(error, "bad magic");
      return false;
    }

    version_ = readU16LE(bytes_.data() + 8);
    if (version_ != 4 && version_ != 5) {
      setError(error, "unsupported version");
      return false;
    }
    is2Bit_ = (readU16LE(bytes_.data() + 10) & 1) != 0;
    const uint8_t styleCount = bytes_[12];
    if (styleCount == 0 || styleCount > 4 || bytes_.size() < 32 + static_cast<size_t>(styleCount) * 32) {
      setError(error, "bad style count");
      return false;
    }

    const uint8_t* toc = bytes_.data() + 32;
    const uint8_t* selected = toc;
    for (uint8_t i = 0; i < styleCount; ++i) {
      const uint8_t* entry = toc + static_cast<size_t>(i) * 32;
      if (entry[0] == 0) {
        selected = entry;
        break;
      }
    }

    styleId_ = selected[0];
    intervalCount_ = readU32LE(selected + 4);
    glyphCount_ = readU32LE(selected + 8);
    advanceY_ = selected[12];
    ascender_ = readI16LE(selected + 13);
    descender_ = readI16LE(selected + 15);
    kernLeftEntryCount_ = readU16LE(selected + 17);
    kernRightEntryCount_ = readU16LE(selected + 19);
    kernLeftClassCount_ = selected[21];
    kernRightClassCount_ = selected[22];
    ligaturePairCount_ = selected[23];
    dataOffset_ = readU32LE(selected + 24);
    verticalSubstitutionCount_ = version_ >= 5 ? readU16LE(selected + 28) : 0;

    if (intervalCount_ > 4096 || glyphCount_ > 65536 || dataOffset_ > bytes_.size()) {
      setError(error, "bad counts");
      return false;
    }

    intervalsOffset_ = dataOffset_;
    glyphsOffset_ = intervalsOffset_ + static_cast<size_t>(intervalCount_) * 12;
    const size_t kernLeftBytes = static_cast<size_t>(kernLeftEntryCount_) * 3;
    const size_t kernRightBytes = static_cast<size_t>(kernRightEntryCount_) * 3;
    const size_t kernMatrixBytes = static_cast<size_t>(kernLeftClassCount_) * kernRightClassCount_;
    const size_t ligatureBytes = static_cast<size_t>(ligaturePairCount_) * 8;
    const size_t verticalBytes = static_cast<size_t>(verticalSubstitutionCount_) * 8;
    bitmapOffset_ = glyphsOffset_ + static_cast<size_t>(glyphCount_) * 16 + kernLeftBytes + kernRightBytes +
                    kernMatrixBytes + ligatureBytes + verticalBytes;

    if (glyphsOffset_ > bytes_.size() || static_cast<size_t>(glyphCount_) * 16 > bytes_.size() - glyphsOffset_ ||
        bitmapOffset_ > bytes_.size()) {
      setError(error, "section out of range");
      return false;
    }
    return true;
  }

  CpGlyph glyphForCodepoint(uint32_t cp) const {
    CpGlyph glyph;
    for (uint32_t i = 0; i < intervalCount_; ++i) {
      const uint8_t* interval = bytes_.data() + intervalsOffset_ + static_cast<size_t>(i) * 12;
      const uint32_t first = readU32LE(interval);
      const uint32_t last = readU32LE(interval + 4);
      const uint32_t offset = readU32LE(interval + 8);
      if (cp < first || cp > last) continue;

      const uint32_t glyphIndex = offset + (cp - first);
      if (glyphIndex >= glyphCount_) return glyph;
      const uint8_t* entry = bytes_.data() + glyphsOffset_ + static_cast<size_t>(glyphIndex) * 16;
      glyph.present = true;
      glyph.width = entry[0];
      glyph.height = entry[1];
      glyph.advanceX = readU16LE(entry + 2);
      glyph.left = readI16LE(entry + 4);
      glyph.top = readI16LE(entry + 6);
      glyph.dataLength = readU16LE(entry + 8);
      glyph.dataOffset = readU32LE(entry + 12);
      if (glyph.dataLength > 0 && glyph.dataOffset <= bytes_.size() - bitmapOffset_ &&
          glyph.dataLength <= bytes_.size() - bitmapOffset_ - glyph.dataOffset) {
        glyph.bitmap = bytes_.data() + bitmapOffset_ + glyph.dataOffset;
      }
      return glyph;
    }
    return glyph;
  }

  const std::string& path() const { return path_; }
  uint16_t version() const { return version_; }
  bool is2Bit() const { return is2Bit_; }
  uint8_t styleId() const { return styleId_; }
  uint32_t glyphCount() const { return glyphCount_; }
  uint32_t intervalCount() const { return intervalCount_; }
  uint8_t advanceY() const { return advanceY_; }
  int16_t ascender() const { return ascender_; }
  int16_t descender() const { return descender_; }

 private:
  static void setError(const char** error, const char* message) {
    if (error) *error = message;
  }

  std::string path_;
  std::vector<uint8_t> bytes_;
  uint16_t version_ = 0;
  bool is2Bit_ = false;
  uint8_t styleId_ = 0;
  uint32_t intervalCount_ = 0;
  uint32_t glyphCount_ = 0;
  uint8_t advanceY_ = 0;
  int16_t ascender_ = 0;
  int16_t descender_ = 0;
  uint16_t kernLeftEntryCount_ = 0;
  uint16_t kernRightEntryCount_ = 0;
  uint8_t kernLeftClassCount_ = 0;
  uint8_t kernRightClassCount_ = 0;
  uint8_t ligaturePairCount_ = 0;
  uint16_t verticalSubstitutionCount_ = 0;
  uint32_t dataOffset_ = 0;
  size_t intervalsOffset_ = 0;
  size_t glyphsOffset_ = 0;
  size_t bitmapOffset_ = 0;
};

BitmapStats statsForStbCoverage(const uint8_t* bitmap, const int width, const int height) {
  BitmapStats stats;
  if (!bitmap || width <= 0 || height <= 0) return stats;
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t coverage = bitmap[i];
    stats.coverageSum += coverage;
    if (coverage >= 64) ++stats.inkPixels;       // FreeType cpfont converter: (v >> 4) >= 4.
    if (coverage > 0 && coverage < 64) ++stats.weakPixels;
  }
  return stats;
}

BitmapStats statsForCustomBitmap(const uint8_t* bitmap, const int width, const int height) {
  BitmapStats stats;
  if (!bitmap || width <= 0 || height <= 0) return stats;
  const size_t pixels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t value = bitmap[i];
    stats.coverageSum += value;
    if (value != 0) ++stats.inkPixels;
  }
  return stats;
}

BitmapStats statsForGrayBitmap(const GrayBitmap& bitmap) {
  BitmapStats stats;
  if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty()) return stats;
  for (const uint8_t gray : bitmap.pixels) {
    const uint8_t coverage = static_cast<uint8_t>(255 - gray);
    stats.coverageSum += coverage;
    if (coverage >= 64) ++stats.inkPixels;
    if (coverage > 0 && coverage < 64) ++stats.weakPixels;
    if (coverage >= 192) ++stats.strongPixels;
  }
  return stats;
}

BitmapStats statsForCpfontBitmap(const CpGlyph& glyph, const bool is2Bit) {
  BitmapStats stats;
  if (!glyph.bitmap || glyph.width == 0 || glyph.height == 0) return stats;
  const size_t pixels = static_cast<size_t>(glyph.width) * glyph.height;
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t coverage = 0;
    if (is2Bit) {
      const uint8_t packed = glyph.bitmap[i >> 2];
      coverage = (packed >> ((3 - (i & 3)) * 2)) & 0x03;
      stats.coverageSum += coverage;
      if (coverage != 0) ++stats.inkPixels;
      if (coverage == 1) ++stats.weakPixels;
      if (coverage == 3) ++stats.strongPixels;
    } else {
      const uint8_t packed = glyph.bitmap[i >> 3];
      coverage = (packed >> (7 - (i & 7))) & 0x01;
      stats.coverageSum += coverage;
      if (coverage != 0) {
        ++stats.inkPixels;
        ++stats.strongPixels;
      }
    }
  }
  return stats;
}

bool looksLikeCpfontPath(const char* path) {
  if (!path) return false;
  const std::string text(path);
  return text.size() >= 7 && text.substr(text.size() - 7) == ".cpfont";
}

bool hasPrefix(const char* text, const char* prefix) {
  return text && prefix && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

void printPreview(const char* label, const uint8_t* bitmap, const int width, const int height, const bool coverage) {
  if (!bitmap || width <= 0 || height <= 0 || width > 80 || height > 80) return;
  std::printf("  %s preview:\n", label);
  for (int y = 0; y < height; ++y) {
    std::printf("  ");
    for (int x = 0; x < width; ++x) {
      const uint8_t value = bitmap[static_cast<size_t>(y) * width + x];
      char ch = '.';
      if (coverage) {
        ch = value >= 192 ? '#' : value >= 128 ? 'O' : value >= 64 ? 'o' : value > 0 ? ':' : '.';
      } else {
        ch = value ? '#' : '.';
      }
      std::putchar(ch);
    }
    std::putchar('\n');
  }
}

void printGrayPreview(const char* label, const GrayBitmap& bitmap) {
  if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty() || bitmap.width > 80 || bitmap.height > 80) {
    return;
  }
  std::printf("  %s preview:\n", label);
  for (int y = 0; y < bitmap.height; ++y) {
    std::printf("  ");
    for (int x = 0; x < bitmap.width; ++x) {
      const uint8_t gray = bitmap.pixels[static_cast<size_t>(y) * bitmap.width + x];
      const uint8_t coverage = static_cast<uint8_t>(255 - gray);
      const char ch = coverage >= 192 ? '#' : coverage >= 128 ? 'O' : coverage >= 64 ? 'o' : coverage > 0 ? ':' : '.';
      std::putchar(ch);
    }
    std::putchar('\n');
  }
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

uint32_t adler32(const std::vector<uint8_t>& data) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : data) {
    a = (a + byte) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void appendU32BE(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void appendChunk(std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& data) {
  appendU32BE(png, static_cast<uint32_t>(data.size()));
  const size_t typeOffset = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  uint32_t crc = crc32Update(0, png.data() + typeOffset, 4 + data.size());
  appendU32BE(png, crc);
}

bool writePngRgb(const char* path, const int width, const int height, const std::vector<uint8_t>& rgb) {
  if (!path || width <= 0 || height <= 0 || rgb.size() != static_cast<size_t>(width) * height * 3) return false;

  std::vector<uint8_t> scanlines;
  scanlines.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
  for (int y = 0; y < height; ++y) {
    scanlines.push_back(0);
    const size_t row = static_cast<size_t>(y) * width * 3;
    scanlines.insert(scanlines.end(), rgb.begin() + row, rgb.begin() + row + static_cast<size_t>(width) * 3);
  }

  std::vector<uint8_t> zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  size_t offset = 0;
  while (offset < scanlines.size()) {
    const size_t blockLen = std::min<size_t>(65535, scanlines.size() - offset);
    zlib.push_back(offset + blockLen == scanlines.size() ? 0x01 : 0x00);
    zlib.push_back(static_cast<uint8_t>(blockLen));
    zlib.push_back(static_cast<uint8_t>(blockLen >> 8));
    const uint16_t nlen = static_cast<uint16_t>(~blockLen);
    zlib.push_back(static_cast<uint8_t>(nlen));
    zlib.push_back(static_cast<uint8_t>(nlen >> 8));
    zlib.insert(zlib.end(), scanlines.begin() + offset, scanlines.begin() + offset + blockLen);
    offset += blockLen;
  }
  appendU32BE(zlib, adler32(scanlines));

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  appendU32BE(ihdr, static_cast<uint32_t>(width));
  appendU32BE(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);
  ihdr.push_back(2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  appendChunk(png, "IHDR", ihdr);
  appendChunk(png, "IDAT", zlib);
  appendChunk(png, "IEND", {});

  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return out.good();
}

void strokeRect(std::vector<uint8_t>& rgb, int canvasW, int canvasH, int x, int y, int w, int h, uint8_t r, uint8_t g,
                uint8_t b);

void blendPixel(std::vector<uint8_t>& rgb, int canvasW, int canvasH, int x, int y, uint8_t gray);

void drawGrayBitmap(std::vector<uint8_t>& rgb, int canvasW, int canvasH, const GrayBitmap& bitmap, int originX,
                    int originY, int scale);

const uint8_t* font5x7(char c) {
  static constexpr uint8_t kBlank[7] = {0, 0, 0, 0, 0, 0, 0};
  static constexpr uint8_t kPlus[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
  static constexpr uint8_t kDash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
  static constexpr uint8_t k0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
  static constexpr uint8_t k1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
  static constexpr uint8_t k2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
  static constexpr uint8_t k3[7] = {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E};
  static constexpr uint8_t k4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
  static constexpr uint8_t k5[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
  static constexpr uint8_t k6[7] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
  static constexpr uint8_t k7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
  static constexpr uint8_t k8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
  static constexpr uint8_t k9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
  static constexpr uint8_t kA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
  static constexpr uint8_t kB[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
  static constexpr uint8_t kC[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
  static constexpr uint8_t kD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
  static constexpr uint8_t kE[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
  static constexpr uint8_t kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
  static constexpr uint8_t kI[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
  static constexpr uint8_t kM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
  static constexpr uint8_t kN[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
  static constexpr uint8_t kO[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  static constexpr uint8_t kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
  static constexpr uint8_t kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
  static constexpr uint8_t kT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
  static constexpr uint8_t kU[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
  static constexpr uint8_t kX[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
  static constexpr uint8_t kZ[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
  switch (c) {
    case '+': return kPlus;
    case '-': return kDash;
    case '0': return k0;
    case '1': return k1;
    case '2': return k2;
    case '3': return k3;
    case '4': return k4;
    case '5': return k5;
    case '6': return k6;
    case '7': return k7;
    case '8': return k8;
    case '9': return k9;
    case 'A': return kA;
    case 'B': return kB;
    case 'C': return kC;
    case 'D': return kD;
    case 'E': return kE;
    case 'F': return kF;
    case 'I': return kI;
    case 'M': return kM;
    case 'N': return kN;
    case 'O': return kO;
    case 'P': return kP;
    case 'S': return kS;
    case 'T': return kT;
    case 'U': return kU;
    case 'X': return kX;
    case 'Z': return kZ;
    default: return kBlank;
  }
}

void drawLabelText(std::vector<uint8_t>& rgb, const int canvasW, const int canvasH, const int x, const int y,
                   const std::string& text, const uint8_t gray = 40) {
  int cursorX = x;
  for (const char raw : text) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
    const uint8_t* rows = font5x7(c);
    for (int gy = 0; gy < 7; ++gy) {
      for (int gx = 0; gx < 5; ++gx) {
        if ((rows[gy] & (1 << (4 - gx))) != 0) {
          blendPixel(rgb, canvasW, canvasH, cursorX + gx, y + gy, gray);
        }
      }
    }
    cursorX += 6;
  }
}

int maxBitmapWidth(const PngGlyphRow& row) {
  return std::max({row.cpfont.width, row.custom1Bit.width, row.custom2xEvenOdd.width, row.custom2xNonZero.width,
                   row.custom3xEvenOdd.width, row.stb.width});
}

int maxBitmapHeight(const PngGlyphRow& row) {
  return std::max({row.cpfont.height, row.custom1Bit.height, row.custom2xEvenOdd.height, row.custom2xNonZero.height,
                   row.custom3xEvenOdd.height, row.stb.height});
}

bool writeComparisonPng(const char* path, const std::vector<PngGlyphRow>& rows) {
  if (!path || rows.empty()) return false;
  constexpr int kColumns = 6;
  constexpr int kScale = 4;
  constexpr int kPad = 12;
  constexpr int kGap = 10;
  constexpr int kHeaderH = 20;
  constexpr int kRowLabelW = 58;
  const char* labels[kColumns] = {"CPFONT", "CUSTOM1", "2XEO", "2XNZ", "3XEO", "STB"};
  int cellW = 1;
  int cellH = 1;
  for (const auto& row : rows) {
    cellW = std::max(cellW, maxBitmapWidth(row) * kScale + kPad * 2);
    cellH = std::max(cellH, maxBitmapHeight(row) * kScale + kPad * 2);
  }
  const int canvasW = kRowLabelW + kColumns * cellW + (kColumns + 1) * kGap;
  const int canvasH = kHeaderH + static_cast<int>(rows.size()) * cellH + (static_cast<int>(rows.size()) + 1) * kGap;
  std::vector<uint8_t> rgb(static_cast<size_t>(canvasW) * canvasH * 3, 245);

  for (int col = 0; col < kColumns; ++col) {
    const int cellX = kRowLabelW + kGap + col * (cellW + kGap);
    drawLabelText(rgb, canvasW, canvasH, cellX + 4, 5, labels[col]);
  }
  for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
    const PngGlyphRow& row = rows[rowIndex];
    const GrayBitmap* bitmaps[kColumns] = {&row.cpfont,         &row.custom1Bit,      &row.custom2xEvenOdd,
                                           &row.custom2xNonZero, &row.custom3xEvenOdd, &row.stb};
    const int cellY = kHeaderH + kGap + static_cast<int>(rowIndex) * (cellH + kGap);
    drawLabelText(rgb, canvasW, canvasH, 4, cellY + 4, cpLabel(row.codepoint));
    for (int col = 0; col < kColumns; ++col) {
      const int cellX = kRowLabelW + kGap + col * (cellW + kGap);
      strokeRect(rgb, canvasW, canvasH, cellX, cellY, cellW, cellH, 180, 180, 180);
      const GrayBitmap& bitmap = *bitmaps[col];
      const int originX = cellX + (cellW - bitmap.width * kScale) / 2;
      const int originY = cellY + (cellH - bitmap.height * kScale) / 2;
      drawGrayBitmap(rgb, canvasW, canvasH, bitmap, originX, originY, kScale);
    }
  }
  return writePngRgb(path, canvasW, canvasH, rgb);
}

void blendPixel(std::vector<uint8_t>& rgb, const int canvasW, const int canvasH, const int x, const int y,
                const uint8_t gray) {
  if (x < 0 || y < 0 || x >= canvasW || y >= canvasH) return;
  const size_t idx = (static_cast<size_t>(y) * canvasW + x) * 3;
  rgb[idx] = gray;
  rgb[idx + 1] = gray;
  rgb[idx + 2] = gray;
}

void strokeRect(std::vector<uint8_t>& rgb, const int canvasW, const int canvasH, const int x, const int y, const int w,
                const int h, const uint8_t r, const uint8_t g, const uint8_t b) {
  for (int px = x; px < x + w; ++px) {
    if (px < 0 || px >= canvasW) continue;
    for (const int py : {y, y + h - 1}) {
      if (py < 0 || py >= canvasH) continue;
      const size_t idx = (static_cast<size_t>(py) * canvasW + px) * 3;
      rgb[idx] = r;
      rgb[idx + 1] = g;
      rgb[idx + 2] = b;
    }
  }
  for (int py = y; py < y + h; ++py) {
    if (py < 0 || py >= canvasH) continue;
    for (const int px : {x, x + w - 1}) {
      if (px < 0 || px >= canvasW) continue;
      const size_t idx = (static_cast<size_t>(py) * canvasW + px) * 3;
      rgb[idx] = r;
      rgb[idx + 1] = g;
      rgb[idx + 2] = b;
    }
  }
}

void drawGrayBitmap(std::vector<uint8_t>& rgb, const int canvasW, const int canvasH, const GrayBitmap& bitmap,
                    const int originX, const int originY, const int scale) {
  if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.pixels.empty()) return;
  for (int y = 0; y < bitmap.height; ++y) {
    for (int x = 0; x < bitmap.width; ++x) {
      const uint8_t gray = bitmap.pixels[static_cast<size_t>(y) * bitmap.width + x];
      for (int sy = 0; sy < scale; ++sy) {
        for (int sx = 0; sx < scale; ++sx) {
          blendPixel(rgb, canvasW, canvasH, originX + x * scale + sx, originY + y * scale + sy, gray);
        }
      }
    }
  }
}

GrayBitmap grayFromCustom1Bit(const uint8_t* bitmap, const ttf::CustomRasterResult& raster) {
  GrayBitmap out;
  if (!bitmap || !raster.ok || raster.width <= 0 || raster.height <= 0) return out;
  out.width = raster.width;
  out.height = raster.height;
  out.xOffset = raster.xOffset;
  out.yOffset = raster.yOffset;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height, 255);
  for (size_t i = 0; i < out.pixels.size(); ++i) {
    out.pixels[i] = bitmap[i] ? 0 : 255;
  }
  return out;
}

GrayBitmap grayFromStb(const uint8_t* bitmap, const ttf::StbRasterResult& raster) {
  GrayBitmap out;
  if (!bitmap || !raster.ok || raster.width <= 0 || raster.height <= 0) return out;
  out.width = raster.width;
  out.height = raster.height;
  out.xOffset = raster.xOffset;
  out.yOffset = raster.yOffset;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height, 255);
  for (size_t i = 0; i < out.pixels.size(); ++i) {
    out.pixels[i] = static_cast<uint8_t>(255 - bitmap[i]);
  }
  return out;
}

GrayBitmap grayFromCpfont(const CpGlyph& glyph, const bool is2Bit) {
  GrayBitmap out;
  if (!glyph.present || !glyph.bitmap || glyph.width == 0 || glyph.height == 0) return out;
  out.width = glyph.width;
  out.height = glyph.height;
  out.xOffset = glyph.left;
  out.yOffset = -glyph.top;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height, 255);
  for (size_t i = 0; i < out.pixels.size(); ++i) {
    uint8_t raw = 0;
    if (is2Bit) {
      raw = (glyph.bitmap[i >> 2] >> ((3 - (i & 3)) * 2)) & 0x03;
      out.pixels[i] = static_cast<uint8_t>(255 - raw * 85);
    } else {
      raw = (glyph.bitmap[i >> 3] >> (7 - (i & 7))) & 0x01;
      out.pixels[i] = raw ? 0 : 255;
    }
  }
  return out;
}

GrayBitmap grayFromSupersampledCoverage(const uint8_t* highBitmap, const ttf::CustomRasterResult& highRaster,
                                        const int factor) {
  GrayBitmap out;
  if (!highBitmap || !highRaster.ok || highRaster.width <= 0 || highRaster.height <= 0 || factor <= 0) return out;
  out.width = (highRaster.width + factor - 1) / factor;
  out.height = (highRaster.height + factor - 1) / factor;
  out.xOffset = highRaster.xOffset / factor;
  out.yOffset = highRaster.yOffset / factor;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height, 255);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      int coverage = 0;
      int samples = 0;
      for (int sy = 0; sy < factor; ++sy) {
        const int hy = y * factor + sy;
        if (hy >= highRaster.height) continue;
        for (int sx = 0; sx < factor; ++sx) {
          const int hx = x * factor + sx;
          if (hx >= highRaster.width) continue;
          ++samples;
          if (highBitmap[static_cast<size_t>(hy) * highRaster.width + hx]) ++coverage;
        }
      }
      const uint8_t raw =
          samples == 0 ? 0 : static_cast<uint8_t>(std::min(3, (coverage * 3 + samples / 2) / samples));
      out.pixels[static_cast<size_t>(y) * out.width + x] = static_cast<uint8_t>(255 - raw * 85);
    }
  }
  return out;
}

void printCpfontPreview(const CpGlyph& glyph, const bool is2Bit) {
  if (!glyph.bitmap || glyph.width == 0 || glyph.height == 0 || glyph.width > 80 || glyph.height > 80) return;
  std::printf("  cpfont preview:\n");
  for (int y = 0; y < glyph.height; ++y) {
    std::printf("  ");
    for (int x = 0; x < glyph.width; ++x) {
      const size_t i = static_cast<size_t>(y) * glyph.width + x;
      uint8_t value = 0;
      if (is2Bit) {
        value = (glyph.bitmap[i >> 2] >> ((3 - (i & 3)) * 2)) & 0x03;
        std::putchar(value == 3 ? '#' : value == 2 ? 'O' : value == 1 ? 'o' : '.');
      } else {
        value = (glyph.bitmap[i >> 3] >> (7 - (i & 7))) & 0x01;
        std::putchar(value ? '#' : '.');
      }
    }
    std::putchar('\n');
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s FONT.ttf PIXEL_SIZE [FONT.cpfont] [--png out.png] [U+0041 U+3042 U+65E5 ...]\n",
                 argv[0]);
    return 2;
  }

  const char* path = argv[1];
  const uint16_t pixelSize = static_cast<uint16_t>(std::max(1, std::atoi(argv[2])));
  const char* cpfontPath = nullptr;
  const char* pngPath = nullptr;
  int codepointArg = 3;
  if (argc >= 4 && looksLikeCpfontPath(argv[3])) {
    cpfontPath = argv[3];
    codepointArg = 4;
  }
  std::vector<uint32_t> codepoints;
  for (int i = codepointArg; i < argc; ++i) {
    if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
      pngPath = argv[++i];
      continue;
    }
    if (hasPrefix(argv[i], "--png=")) {
      pngPath = argv[i] + 6;
      continue;
    }
    const uint32_t cp = parseCodepoint(argv[i]);
    if (cp != 0) codepoints.push_back(cp);
  }
  if (codepoints.empty()) {
    codepoints = {0x0041, 0x3042, 0x65E5, 0x3001};
  }

  std::vector<uint8_t> bytes;
  if (!loadFile(path, bytes)) {
    std::fprintf(stderr, "failed to open %s\n", path);
    return 2;
  }

  const auto sfnt = ttf::probeSfnt(bytes.data(), bytes.size());
  if (!sfnt.ok) {
    std::fprintf(stderr, "sfnt probe failed: %s\n", sfnt.error ? sfnt.error : "unknown");
    return 1;
  }

  const ttf::RuntimeTableSet tables = {
      tableView(bytes, ttf::tag('c', 'm', 'a', 'p')), tableView(bytes, ttf::tag('h', 'e', 'a', 'd')),
      tableView(bytes, ttf::tag('h', 'h', 'e', 'a')), tableView(bytes, ttf::tag('h', 'm', 't', 'x')),
      tableView(bytes, ttf::tag('l', 'o', 'c', 'a')), tableView(bytes, ttf::tag('m', 'a', 'x', 'p')),
  };
  const ttf::TableView glyf = tableView(bytes, ttf::tag('g', 'l', 'y', 'f'));
  ttf::TtfRuntimeFont runtimeFont;
  const char* metricsError = nullptr;
  if (!runtimeFont.begin(tables, pixelSize, &metricsError)) {
    std::fprintf(stderr, "runtime metrics failed: %s\n", metricsError ? metricsError : "unknown");
    return 1;
  }
  if (!glyf.data) {
    std::fprintf(stderr, "missing glyf table\n");
    return 1;
  }

  std::vector<uint8_t> stbBitmap(kBitmapScratchBytes);
  std::vector<uint8_t> customBitmap(kBitmapScratchBytes);
  std::vector<uint8_t> custom2xEvenOddBitmap(kBitmapScratchBytes);
  std::vector<uint8_t> custom2xNonZeroBitmap(kBitmapScratchBytes);
  std::vector<uint8_t> custom3xEvenOddBitmap(kBitmapScratchBytes);
  CpFont cpfont;
  bool haveCpfont = false;
  if (cpfontPath) {
    const char* cpfontError = nullptr;
    haveCpfont = cpfont.load(cpfontPath, &cpfontError);
    if (!haveCpfont) {
      std::fprintf(stderr, "cpfont load failed: %s\n", cpfontError ? cpfontError : "unknown");
      return 1;
    }
  }

  const auto& metrics = runtimeFont.metrics();
  std::printf("font=%s px=%u upem=%u asc=%d desc=%d line_gap=%d tables=%u\n", path, pixelSize, metrics.unitsPerEm,
              metrics.ascender, metrics.descender, metrics.lineGap, sfnt.tableCount);
  if (haveCpfont) {
    std::printf("cpfont=%s version=%u style=%u format=%s glyphs=%u intervals=%u advanceY=%u asc=%d desc=%d\n",
                cpfont.path().c_str(), cpfont.version(), cpfont.styleId(), cpfont.is2Bit() ? "2bit" : "1bit",
                cpfont.glyphCount(), cpfont.intervalCount(), cpfont.advanceY(), cpfont.ascender(), cpfont.descender());
  }
  std::printf("cp, present, glyph, adv_px, lsb_px, stb_box, stb_ink64, stb_density, stb_weak, custom_box, "
              "custom_ink, custom_density, custom_vs_stb_ink, custom_2x2bit_box, custom_2x2bit_ink64, "
              "custom_2x2bit_density, custom_2x2bit_weak, custom_2x2bit_strong, custom_2x2bit_vs_stb_ink, "
              "cpfont_box, cpfont_adv_px, cpfont_ink, cpfont_density, cpfont_weak, cpfont_strong, "
              "custom_vs_cpfont_ink, custom_2x2bit_vs_cpfont_ink\n");

  std::vector<PngGlyphRow> pngRows;
  for (const uint32_t cp : codepoints) {
    std::fill(stbBitmap.begin(), stbBitmap.end(), 0);
    std::fill(customBitmap.begin(), customBitmap.end(), 0);
    std::fill(custom2xEvenOddBitmap.begin(), custom2xEvenOddBitmap.end(), 0);
    std::fill(custom2xNonZeroBitmap.begin(), custom2xNonZeroBitmap.end(), 0);
    std::fill(custom3xEvenOddBitmap.begin(), custom3xEvenOddBitmap.end(), 0);

    const auto glyph = runtimeFont.metricsForCodepoint(cp);
    if (!glyph.present) {
      std::printf("%s, no, 0, 0, 0, -, 0, 0, -, 0, -, -, -, -, -, -, -, -\n", cpLabel(cp).c_str());
      continue;
    }

    const auto stb = ttf::rasterizeStbGlyph(bytes.data(), bytes.size(), cp, static_cast<float>(pixelSize),
                                            stbBitmap.data(), stbBitmap.size());
    ttf::CustomRasterResult custom{};
    ttf::CustomRasterResult custom2xEvenOdd{};
    ttf::CustomRasterResult custom2xNonZero{};
    ttf::CustomRasterResult custom3xEvenOdd{};
    if (glyph.glyphOffset <= glyf.length && glyph.glyphLength <= glyf.length - glyph.glyphOffset) {
      custom = ttf::rasterizeSimpleGlyf(glyf.data + glyph.glyphOffset, glyph.glyphLength, metrics.unitsPerEm,
                                        pixelSize, customBitmap.data(), customBitmap.size());
      custom2xEvenOdd = ttf::rasterizeSimpleGlyfWithFillRule(
          glyf.data + glyph.glyphOffset, glyph.glyphLength, metrics.unitsPerEm, static_cast<uint16_t>(pixelSize * 2),
          ttf::CustomFillRule::EvenOdd, custom2xEvenOddBitmap.data(), custom2xEvenOddBitmap.size());
      custom2xNonZero = ttf::rasterizeSimpleGlyfWithFillRule(
          glyf.data + glyph.glyphOffset, glyph.glyphLength, metrics.unitsPerEm, static_cast<uint16_t>(pixelSize * 2),
          ttf::CustomFillRule::NonZero, custom2xNonZeroBitmap.data(), custom2xNonZeroBitmap.size());
      custom3xEvenOdd = ttf::rasterizeSimpleGlyfWithFillRule(
          glyf.data + glyph.glyphOffset, glyph.glyphLength, metrics.unitsPerEm, static_cast<uint16_t>(pixelSize * 3),
          ttf::CustomFillRule::EvenOdd, custom3xEvenOddBitmap.data(), custom3xEvenOddBitmap.size());
    }

    const BitmapStats stbStats = stb.ok ? statsForStbCoverage(stbBitmap.data(), stb.width, stb.height) : BitmapStats{};
    const BitmapStats customStats =
        custom.ok ? statsForCustomBitmap(customBitmap.data(), custom.width, custom.height) : BitmapStats{};
    const double ratio = stbStats.inkPixels > 0 ? static_cast<double>(customStats.inkPixels) / stbStats.inkPixels : 0.0;
    const GrayBitmap custom2BitGray = grayFromSupersampledCoverage(custom2xEvenOddBitmap.data(), custom2xEvenOdd, 2);
    const BitmapStats custom2BitStats = statsForGrayBitmap(custom2BitGray);
    const double custom2BitStbRatio =
        stbStats.inkPixels > 0 ? static_cast<double>(custom2BitStats.inkPixels) / stbStats.inkPixels : 0.0;
    const CpGlyph cpGlyph = haveCpfont ? cpfont.glyphForCodepoint(cp) : CpGlyph{};
    const BitmapStats cpfontStats = cpGlyph.present ? statsForCpfontBitmap(cpGlyph, cpfont.is2Bit()) : BitmapStats{};

    std::printf("%s, yes, %u, %d, %d, ", cpLabel(cp).c_str(), glyph.glyphId, glyph.advancePx,
                glyph.leftSideBearingPx);
    if (stb.ok) {
      const double stbDensity =
          stb.width > 0 && stb.height > 0
              ? static_cast<double>(stbStats.inkPixels) / (static_cast<double>(stb.width) * stb.height)
              : 0.0;
      std::printf("%dx%d %+d%+d, %zu, %.3f, %zu, ", stb.width, stb.height, stb.xOffset, stb.yOffset,
                  stbStats.inkPixels, stbDensity, stbStats.weakPixels);
    } else {
      std::printf("error:%s, 0, 0, 0, ", stb.error ? stb.error : "unknown");
    }
    if (custom.ok) {
      const double customDensity =
          custom.width > 0 && custom.height > 0
              ? static_cast<double>(customStats.inkPixels) / (static_cast<double>(custom.width) * custom.height)
              : 0.0;
      std::printf("%dx%d %+d%+d, %zu, %.3f, %.3f, ", custom.width, custom.height, custom.xOffset, custom.yOffset,
                  customStats.inkPixels, customDensity, ratio);
    } else {
      std::printf("error:%s, 0, -, ", custom.error ? custom.error : "unknown");
    }
    if (!custom2BitGray.pixels.empty()) {
      const double custom2BitDensity =
          custom2BitGray.width > 0 && custom2BitGray.height > 0
              ? static_cast<double>(custom2BitStats.inkPixels) /
                    (static_cast<double>(custom2BitGray.width) * custom2BitGray.height)
              : 0.0;
      std::printf("%dx%d %+d%+d, %zu, %.3f, %zu, %zu, %.3f, ", custom2BitGray.width, custom2BitGray.height,
                  custom2BitGray.xOffset, custom2BitGray.yOffset, custom2BitStats.inkPixels, custom2BitDensity,
                  custom2BitStats.weakPixels, custom2BitStats.strongPixels, custom2BitStbRatio);
    } else {
      std::printf("error:%s, 0, 0, 0, 0, -, ", custom2xEvenOdd.error ? custom2xEvenOdd.error : "unknown");
    }
    if (haveCpfont && cpGlyph.present) {
      const double cpfontDensity =
          cpGlyph.width > 0 && cpGlyph.height > 0
              ? static_cast<double>(cpfontStats.inkPixels) / (static_cast<double>(cpGlyph.width) * cpGlyph.height)
              : 0.0;
      const double customCpfontRatio =
          cpfontStats.inkPixels > 0 ? static_cast<double>(customStats.inkPixels) / cpfontStats.inkPixels : 0.0;
      const double custom2BitCpfontRatio =
          cpfontStats.inkPixels > 0 ? static_cast<double>(custom2BitStats.inkPixels) / cpfontStats.inkPixels : 0.0;
      std::printf("%ux%u %+d%+d, %.1f, %zu, %.3f, %zu, %zu, %.3f, %.3f\n", cpGlyph.width, cpGlyph.height,
                  cpGlyph.left, cpGlyph.top, static_cast<double>(cpGlyph.advanceX) / 16.0, cpfontStats.inkPixels,
                  cpfontDensity, cpfontStats.weakPixels, cpfontStats.strongPixels, customCpfontRatio,
                  custom2BitCpfontRatio);
    } else if (haveCpfont) {
      std::printf("missing, 0, 0, 0, 0, 0, 0, 0\n");
    } else {
      std::printf("-, -, -, -, -, -, -, -\n");
    }

    if (stb.ok && custom.ok && (stb.width <= 48 || custom.width <= 48) && (stb.height <= 48 || custom.height <= 48)) {
      printPreview("stb", stbBitmap.data(), stb.width, stb.height, true);
      printPreview("custom", customBitmap.data(), custom.width, custom.height, false);
      if (!custom2BitGray.pixels.empty()) {
        printGrayPreview("custom-2x-2bit", custom2BitGray);
      }
      if (haveCpfont && cpGlyph.present) printCpfontPreview(cpGlyph, cpfont.is2Bit());
    }

    if (pngPath) {
      PngGlyphRow row;
      row.codepoint = cp;
      if (haveCpfont && cpGlyph.present) row.cpfont = grayFromCpfont(cpGlyph, cpfont.is2Bit());
      row.custom1Bit = grayFromCustom1Bit(customBitmap.data(), custom);
      row.custom2xEvenOdd = custom2BitGray;
      row.custom2xNonZero = grayFromSupersampledCoverage(custom2xNonZeroBitmap.data(), custom2xNonZero, 2);
      row.custom3xEvenOdd = grayFromSupersampledCoverage(custom3xEvenOddBitmap.data(), custom3xEvenOdd, 3);
      row.stb = grayFromStb(stbBitmap.data(), stb);
      pngRows.push_back(std::move(row));
    }
  }

  if (pngPath) {
    if (!writeComparisonPng(pngPath, pngRows)) {
      std::fprintf(stderr, "failed to write png %s\n", pngPath);
      return 1;
    }
    std::fprintf(stderr, "wrote %s (columns: cpfont, custom-1bit, 2x-evenodd, 2x-nonzero, 3x-evenodd, stb)\n",
                 pngPath);
  }

  return 0;
}
