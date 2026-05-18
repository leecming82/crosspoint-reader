#include "EpubReaderNavigation.h"

#include <algorithm>

#include "util/StringUtils.h"

namespace {
std::string chapterFallback(const int tocIndex) { return "Chapter " + std::to_string(tocIndex + 1); }

std::string sectionTitle(const std::string& title, const uint16_t sectionIndex) {
  return title + " - Section " + std::to_string(sectionIndex);
}

int nextForwardTocSpine(const std::shared_ptr<Epub>& epub, const int tocIndex, const int startSpineIndex) {
  const int tocCount = epub->getTocItemsCount();
  for (int i = tocIndex + 1; i < tocCount; i++) {
    const auto tocItem = epub->getTocItem(i);
    if (tocItem.spineIndex > startSpineIndex) {
      return tocItem.spineIndex;
    }
  }
  return epub->getSpineItemsCount();
}

bool tocTargetsSpine(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  const int tocCount = epub->getTocItemsCount();
  for (int tocIndex = 0; tocIndex < tocCount; tocIndex++) {
    if (epub->getTocItem(tocIndex).spineIndex == spineIndex) {
      return true;
    }
  }
  return false;
}

bool shouldAddGeneratedPart(const std::shared_ptr<Epub>& epub, const int spineIndex, const int tocSpineIndex,
                            const std::vector<bool>* tocTargetMap) {
  if (spineIndex == tocSpineIndex) {
    return false;
  }
  if (tocTargetMap && spineIndex >= 0 && spineIndex < static_cast<int>(tocTargetMap->size()) &&
      (*tocTargetMap)[spineIndex]) {
    return false;
  }
  if (!tocTargetMap && tocTargetsSpine(epub, spineIndex)) {
    return false;
  }

  const auto spine = epub->getSpineItem(spineIndex);
  return spine.splitCount > 1;
}

std::vector<int> generatedPartSpinesForTocInterval(const std::shared_ptr<Epub>& epub, const int tocIndex,
                                                   const int tocSpineIndex,
                                                   const std::vector<bool>* tocTargetMap = nullptr) {
  std::vector<int> partSpines;
  if (!epub || tocSpineIndex < 0 || tocSpineIndex >= epub->getSpineItemsCount()) {
    return partSpines;
  }

  const int nextSpineIndex = nextForwardTocSpine(epub, tocIndex, tocSpineIndex);
  const int spineCount = epub->getSpineItemsCount();
  for (int spineIndex = tocSpineIndex + 1; spineIndex < nextSpineIndex && spineIndex < spineCount; spineIndex++) {
    if (shouldAddGeneratedPart(epub, spineIndex, tocSpineIndex, tocTargetMap)) {
      partSpines.push_back(spineIndex);
    }
  }
  return partSpines;
}
}  // namespace

namespace EpubReaderNavigation {

std::vector<Entry> buildEntries(const std::shared_ptr<Epub>& epub) {
  std::vector<Entry> entries;
  if (!epub) {
    return entries;
  }

  const int tocCount = epub->getTocItemsCount();
  const int spineCount = epub->getSpineItemsCount();
  entries.reserve(tocCount);

  std::vector<bool> tocTargetsSpine(spineCount, false);
  for (int tocIndex = 0; tocIndex < tocCount; tocIndex++) {
    const auto tocItem = epub->getTocItem(tocIndex);
    if (tocItem.spineIndex >= 0 && tocItem.spineIndex < spineCount) {
      tocTargetsSpine[tocItem.spineIndex] = true;
    }
  }

  for (int tocIndex = 0; tocIndex < tocCount; tocIndex++) {
    const auto tocItem = epub->getTocItem(tocIndex);
    const std::string title = StringUtils::uiSafeLabelOrFallback(tocItem.title, chapterFallback(tocIndex));

    if (tocItem.spineIndex < 0 || tocItem.spineIndex >= spineCount) {
      entries.push_back({title, tocItem.level, tocItem.spineIndex});
      continue;
    }

    const auto partSpines = generatedPartSpinesForTocInterval(epub, tocIndex, tocItem.spineIndex, &tocTargetsSpine);
    const bool tocTargetIsSplit = epub->getSpineItem(tocItem.spineIndex).splitCount > 1;
    entries.push_back({tocTargetIsSplit ? sectionTitle(title, 1) : title, tocItem.level, tocItem.spineIndex});
    for (size_t i = 0; i < partSpines.size(); i++) {
      const uint16_t sectionIndex = static_cast<uint16_t>(i + (tocTargetIsSplit ? 2 : 1));
      entries.push_back({sectionTitle(title, sectionIndex), tocItem.level, partSpines[i]});
    }
  }

  return entries;
}

std::string titleForSpine(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& fallback) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return fallback;
  }

  const int tocCount = epub->getTocItemsCount();
  const auto spine = epub->getSpineItem(spineIndex);

  for (int tocIndex = 0; tocIndex < tocCount; tocIndex++) {
    const auto tocItem = epub->getTocItem(tocIndex);
    if (tocItem.spineIndex == spineIndex) {
      const std::string title = StringUtils::uiSafeLabelOrFallback(tocItem.title, chapterFallback(tocIndex));
      return spine.splitCount > 1 ? sectionTitle(title, 1) : title;
    }
  }

  if (spine.splitCount > 1) {
    for (int tocIndex = 0; tocIndex < tocCount; tocIndex++) {
      const auto tocItem = epub->getTocItem(tocIndex);
      const auto partSpines = generatedPartSpinesForTocInterval(epub, tocIndex, tocItem.spineIndex);
      const auto found = std::find(partSpines.begin(), partSpines.end(), spineIndex);
      if (found == partSpines.end()) {
        continue;
      }

      const std::string title = StringUtils::uiSafeLabelOrFallback(tocItem.title, chapterFallback(tocIndex));
      const bool tocTargetIsSplit = tocItem.spineIndex >= 0 && tocItem.spineIndex < epub->getSpineItemsCount() &&
                                    epub->getSpineItem(tocItem.spineIndex).splitCount > 1;
      const uint16_t sectionIndex = static_cast<uint16_t>((found - partSpines.begin()) + (tocTargetIsSplit ? 2 : 1));
      return sectionTitle(title, sectionIndex);
    }
  }

  const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  std::string title = tocIndex >= 0 ? chapterFallback(tocIndex) : fallback;
  if (tocIndex >= 0 && tocIndex < epub->getTocItemsCount()) {
    const auto tocItem = epub->getTocItem(tocIndex);
    title = StringUtils::uiSafeLabelOrFallback(tocItem.title, title);
  }

  if (spine.splitCount > 1) {
    title = sectionTitle(title, spine.splitIndex + 1);
  }
  return title;
}

}  // namespace EpubReaderNavigation
