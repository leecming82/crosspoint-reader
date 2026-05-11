#pragma once

#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxBytes bytes.
 */
std::string sanitizeFilename(const std::string& name, size_t maxBytes = 100);

bool hasUnsupportedUiCodepoint(const std::string& text);
std::string uiSafeTextWithMarkers(const std::string& text);
std::string basenameWithoutExtension(const std::string& path);
std::string uiSafeBookTitle(const std::string& title, const std::string& path);
std::string uiSafeAuthor(const std::string& author);
std::string uiSafeLabelOrFallback(const std::string& label, const std::string& fallback);

}  // namespace StringUtils
