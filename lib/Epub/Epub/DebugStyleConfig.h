#pragma once

#include <cstdint>

enum class EmbeddedEpubCssMode : uint8_t {
  Disabled,        // Ignore all EPUB-authored CSS.
  TypographyOnly,  // Keep text-facing CSS, drop layout-heavy rules.
  Full,            // Apply every property supported by CssParser.
};

// Temporary diagnostics switch: keep the useful presentation bits while avoiding
// publisher layout rules that can consume heap or collapse the reader layout.
constexpr EmbeddedEpubCssMode DEBUG_EMBEDDED_EPUB_CSS_MODE = EmbeddedEpubCssMode::TypographyOnly;
constexpr bool DEBUG_IGNORE_EMBEDDED_EPUB_CSS = DEBUG_EMBEDDED_EPUB_CSS_MODE == EmbeddedEpubCssMode::Disabled;
constexpr bool DEBUG_FILTER_EMBEDDED_EPUB_CSS_TO_TYPOGRAPHY =
    DEBUG_EMBEDDED_EPUB_CSS_MODE == EmbeddedEpubCssMode::TypographyOnly;
