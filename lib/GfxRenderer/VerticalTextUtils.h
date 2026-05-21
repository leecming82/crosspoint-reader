#pragma once

#include <cstdint>

namespace VerticalTextUtils {

enum class VerticalBehavior : uint8_t {
  Upright = 0,
  Sideways = 1,
  TateChuYoko = 2,
};

inline bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

inline bool isCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK symbols and punctuation
         (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||  // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK unified ideographs
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||  // Fullwidth forms
         (cp >= 0x20000 && cp <= 0x2FA1F);
}

inline bool isUprightInVertical(const uint32_t cp) {
  if (isCjkCodepoint(cp)) return true;
  switch (cp) {
    case 0x002D:  // hyphen-minus
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:  // horizontal bar
    case 0x2025:  // two dot leader
    case 0x2026:  // ellipsis
    case 0x30FC:  // katakana-hiragana prolonged sound mark
    case 0xFF0D:  // fullwidth hyphen-minus
      return true;
    default:
      return false;
  }
}

inline bool isKinsokuHead(const uint32_t cp) {
  switch (cp) {
    case 0x0021:
    case 0x0025:
    case 0x0029:
    case 0x002C:
    case 0x002E:
    case 0x003F:
    case 0x005D:
    case 0x007D:
    case 0x2019:
    case 0x201D:
    case 0x2026:
    case 0x3001:
    case 0x3002:
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
      return true;
    default:
      return false;
  }
}

inline bool isKinsokuTail(const uint32_t cp) {
  switch (cp) {
    case 0x0028:
    case 0x005B:
    case 0x007B:
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

}  // namespace VerticalTextUtils
