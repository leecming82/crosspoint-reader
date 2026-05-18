#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace EpubReaderNavigation {

struct Entry {
  std::string title;
  uint8_t level = 1;
  int spineIndex = -1;
};

std::vector<Entry> buildEntries(const std::shared_ptr<Epub>& epub);
std::string titleForSpine(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& fallback);

}  // namespace EpubReaderNavigation
