#pragma once

#include <string>

namespace jpdict {

struct RomajiComposition {
  std::string committed;
  std::string pending;
};

std::string romajiToHiragana(const std::string& input);
RomajiComposition composeRomaji(const std::string& input, bool final = false);

}  // namespace jpdict
