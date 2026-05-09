#pragma once

// Temporary diagnostics switch: ignore all EPUB-authored CSS so we can compare
// Japanese layout behavior without publisher styles consuming heap or changing
// block metrics.
constexpr bool DEBUG_IGNORE_EMBEDDED_EPUB_CSS = true;
