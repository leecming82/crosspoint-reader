#pragma once

#include <cstdint>

enum class EpubWritingMode : uint8_t {
  HorizontalTb = 0,
  VerticalRl = 1,
  VerticalLr = 2,
};

inline const char* epubWritingModeName(const EpubWritingMode mode) {
  switch (mode) {
    case EpubWritingMode::VerticalRl:
      return "vertical-rl";
    case EpubWritingMode::VerticalLr:
      return "vertical-lr";
    case EpubWritingMode::HorizontalTb:
    default:
      return "horizontal-tb";
  }
}
