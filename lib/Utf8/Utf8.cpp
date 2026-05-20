#include "Utf8.h"

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;          // 0xxxxxxx
  if ((c >> 5) == 0x6) return 2;   // 110xxxxx
  if ((c >> 4) == 0xE) return 3;   // 1110xxxx
  if ((c >> 3) == 0x1E) return 4;  // 11110xxx
  return 1;                        // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
  if (**string == 0) {
    return 0;
  }

  const unsigned char lead = **string;
  const int bytes = utf8CodepointLen(lead);
  const uint8_t* chr = *string;

  // Invalid lead byte (stray continuation byte 0x80-0xBF, or 0xFE/0xFF)
  if (bytes == 1 && lead >= 0x80) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  if (bytes == 1) {
    (*string)++;
    return chr[0];
  }

  // Validate continuation bytes before consuming them
  for (int i = 1; i < bytes; i++) {
    if ((chr[i] & 0xC0) != 0x80) {
      // Missing or invalid continuation byte — skip all bytes consumed so far
      *string += i;
      return REPLACEMENT_GLYPH;
    }
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);  // mask header bits

  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  // Reject overlong encodings, surrogates, and out-of-range values
  const bool overlong = (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000);
  const bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  if (overlong || surrogate || cp > 0x10FFFF) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  *string += bytes;

  return cp;
}

int utf8SafeTruncateBuffer(const char* buf, int len) {
  if (len <= 0) return 0;

  // Walk back past continuation bytes (10xxxxxx) to find the lead byte
  int leadPos = len - 1;
  while (leadPos > 0 && (static_cast<uint8_t>(buf[leadPos]) & 0xC0) == 0x80) {
    leadPos--;
  }

  // Determine expected length of the sequence starting at leadPos
  int expectedLen = utf8CodepointLen(static_cast<unsigned char>(buf[leadPos]));
  int actualLen = len - leadPos;

  if (actualLen < expectedLen && leadPos > 0) {
    // Incomplete UTF-8 sequence at the end — exclude it
    return leadPos;
  }
  return len;
}

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}

namespace {

constexpr uint32_t COMBINING_DAKUTEN = 0x3099;
constexpr uint32_t COMBINING_HANDAKUTEN = 0x309A;

struct KanaComposition {
  uint32_t base;
  uint32_t composed;
};

constexpr KanaComposition DAKUTEN_TABLE[] = {
    {0x304B, 0x304C}, {0x304D, 0x304E}, {0x304F, 0x3050}, {0x3051, 0x3052}, {0x3053, 0x3054}, {0x3055, 0x3056},
    {0x3057, 0x3058}, {0x3059, 0x305A}, {0x305B, 0x305C}, {0x305D, 0x305E}, {0x305F, 0x3060}, {0x3061, 0x3062},
    {0x3064, 0x3065}, {0x3066, 0x3067}, {0x3068, 0x3069}, {0x306F, 0x3070}, {0x3072, 0x3073}, {0x3075, 0x3076},
    {0x3078, 0x3079}, {0x307B, 0x307C}, {0x3046, 0x3094}, {0x30AB, 0x30AC}, {0x30AD, 0x30AE}, {0x30AF, 0x30B0},
    {0x30B1, 0x30B2}, {0x30B3, 0x30B4}, {0x30B5, 0x30B6}, {0x30B7, 0x30B8}, {0x30B9, 0x30BA}, {0x30BB, 0x30BC},
    {0x30BD, 0x30BE}, {0x30BF, 0x30C0}, {0x30C1, 0x30C2}, {0x30C4, 0x30C5}, {0x30C6, 0x30C7}, {0x30C8, 0x30C9},
    {0x30CF, 0x30D0}, {0x30D2, 0x30D3}, {0x30D5, 0x30D6}, {0x30D8, 0x30D9}, {0x30DB, 0x30DC}, {0x30A6, 0x30F4},
    {0x30EF, 0x30F7}, {0x30F0, 0x30F8}, {0x30F1, 0x30F9}, {0x30F2, 0x30FA},
};

constexpr KanaComposition HANDAKUTEN_TABLE[] = {
    {0x306F, 0x3071}, {0x3072, 0x3074}, {0x3075, 0x3077}, {0x3078, 0x307A}, {0x307B, 0x307D},
    {0x30CF, 0x30D1}, {0x30D2, 0x30D4}, {0x30D5, 0x30D7}, {0x30D8, 0x30DA}, {0x30DB, 0x30DD},
};

uint32_t lookupComposition(const uint32_t base, const KanaComposition* table, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (table[i].base == base) return table[i].composed;
  }
  return 0;
}

void encodeUtf8Bmp(char* dst, const uint32_t cp) {
  dst[0] = static_cast<char>(0xE0 | (cp >> 12));
  dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
}

}  // namespace

void utf8NfcNormalizeKana(std::string& str) {
  const size_t len = str.size();
  if (len < 6) return;

  size_t readPos = 0;
  size_t writePos = 0;
  uint32_t prevCp = 0;
  size_t prevCpWritePos = 0;
  int prevCpLen = 0;

  while (readPos < len) {
    const auto lead = static_cast<uint8_t>(str[readPos]);
    const int cpLen = utf8CodepointLen(lead);
    if (readPos + cpLen > len) break;

    uint32_t cp;
    if (cpLen == 1) {
      cp = lead;
    } else if (cpLen == 2) {
      cp = (lead & 0x1F) << 6 | (static_cast<uint8_t>(str[readPos + 1]) & 0x3F);
    } else if (cpLen == 3) {
      cp = (lead & 0x0F) << 12 | (static_cast<uint8_t>(str[readPos + 1]) & 0x3F) << 6 |
           (static_cast<uint8_t>(str[readPos + 2]) & 0x3F);
    } else {
      cp = (lead & 0x07) << 18 | (static_cast<uint8_t>(str[readPos + 1]) & 0x3F) << 12 |
           (static_cast<uint8_t>(str[readPos + 2]) & 0x3F) << 6 | (static_cast<uint8_t>(str[readPos + 3]) & 0x3F);
    }

    if (prevCpLen > 0 && (cp == COMBINING_DAKUTEN || cp == COMBINING_HANDAKUTEN)) {
      const auto* table = (cp == COMBINING_DAKUTEN) ? DAKUTEN_TABLE : HANDAKUTEN_TABLE;
      const size_t count = (cp == COMBINING_DAKUTEN) ? (sizeof(DAKUTEN_TABLE) / sizeof(DAKUTEN_TABLE[0]))
                                                     : (sizeof(HANDAKUTEN_TABLE) / sizeof(HANDAKUTEN_TABLE[0]));
      const uint32_t composed = lookupComposition(prevCp, table, count);
      if (composed != 0) {
        encodeUtf8Bmp(&str[prevCpWritePos], composed);
        readPos += cpLen;
        prevCp = composed;
        continue;
      }
    }

    prevCp = cp;
    prevCpWritePos = writePos;
    prevCpLen = cpLen;
    if (readPos != writePos) {
      for (int i = 0; i < cpLen; ++i) {
        str[writePos + i] = str[readPos + i];
      }
    }
    writePos += cpLen;
    readPos += cpLen;
  }

  if (writePos < len) {
    str.resize(writePos);
  }
}
