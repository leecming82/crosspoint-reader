#include "StringUtils.h"

#include <CjkUiFontFallback.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdint>

namespace StringUtils {
namespace {

bool isUnsupportedUiCodepoint(uint32_t cp) {
  if (cjkUiHasAnyGlyph(cp)) {
    return false;
  }

  return (cp >= 0x2E80 && cp <= 0x2EFF) ||  // CJK radicals
         (cp >= 0x2F00 && cp <= 0x2FDF) ||  // Kangxi radicals
         (cp >= 0x3000 && cp <= 0x303F) ||  // CJK symbols and punctuation
         (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana, Katakana
         (cp >= 0x31F0 && cp <= 0x31FF) ||  // Katakana phonetic extensions
         (cp >= 0x3400 && cp <= 0x9FFF) ||  // CJK ideographs
         (cp >= 0xAC00 && cp <= 0xD7AF) ||  // Hangul syllables
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
         (cp >= 0xFF00 && cp <= 0xFFEF) ||  // Fullwidth forms / halfwidth kana
         (cp >= 0x20000 && cp <= 0x2FA1F);  // CJK extensions
}

std::string trimAsciiSpacesAndDots(std::string text) {
  const auto start = text.find_first_not_of(" .");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = text.find_last_not_of(" .");
  return text.substr(start, end - start + 1);
}

constexpr const char* UNSUPPORTED_UI_GLYPH = "\xE2\x96\xA1";  // WHITE SQUARE

std::string normalizedForUiDisplay(std::string text) {
  size_t pos = 0;
  while ((pos = text.find("\xE3\x80\x80", pos)) != std::string::npos) {
    text.replace(pos, 3, " ");
    pos += 1;
  }
  utf8NfcNormalizeKana(text);
  return text;
}

std::string uiSafeNormalizedTextWithMarkers(const std::string& normalized) {
  std::string result;
  result.reserve(normalized.size());

  const auto* cursor = reinterpret_cast<const unsigned char*>(normalized.c_str());
  while (*cursor != 0) {
    const auto* cpStart = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);

    if (isUnsupportedUiCodepoint(cp)) {
      result += UNSUPPORTED_UI_GLYPH;
      continue;
    }

    result.append(reinterpret_cast<const char*>(cpStart), cursor - cpStart);
  }

  return result;
}

}  // namespace

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }

  return result.empty() ? "book" : result;
}

bool hasUnsupportedUiCodepoint(const std::string& text) {
  const std::string normalized = normalizedForUiDisplay(text);
  const auto* cursor = reinterpret_cast<const unsigned char*>(normalized.c_str());
  while (*cursor != 0) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (isUnsupportedUiCodepoint(cp)) {
      return true;
    }
  }
  return false;
}

std::string uiSafeTextWithMarkers(const std::string& text) {
  const std::string normalized = normalizedForUiDisplay(text);
  return uiSafeNormalizedTextWithMarkers(normalized);
}

std::string basenameWithoutExtension(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  std::string name = path.substr(start);
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    name.resize(dot);
  }
  name = normalizedForUiDisplay(name);
  name = trimAsciiSpacesAndDots(name);
  return name.empty() ? "Book" : name;
}

std::string uiSafeBookTitle(const std::string& title, const std::string& path) {
  const std::string normalizedTitle = normalizedForUiDisplay(title);
  if (!normalizedTitle.empty() && !hasUnsupportedUiCodepoint(normalizedTitle)) {
    return normalizedTitle;
  }

  const std::string filename = basenameWithoutExtension(path);
  if (!filename.empty() && !hasUnsupportedUiCodepoint(filename)) {
    return filename;
  }

  return uiSafeTextWithMarkers(filename);
}

std::string uiSafeAuthor(const std::string& author) {
  const std::string normalized = normalizedForUiDisplay(author);
  return uiSafeNormalizedTextWithMarkers(normalized);
}

std::string uiSafeLabelOrFallback(const std::string& label, const std::string& fallback) {
  const std::string normalizedLabel = normalizedForUiDisplay(label);
  if (!normalizedLabel.empty()) {
    return uiSafeNormalizedTextWithMarkers(normalizedLabel);
  }
  return uiSafeTextWithMarkers(fallback);
}

}  // namespace StringUtils
