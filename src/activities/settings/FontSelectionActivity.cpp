#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <climits>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchList.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {

std::string basenameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry)
    : Activity("FontSelect", renderer, mappedInput), registry_(registry) {}

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry, const bool returnSelectionOnly,
                                             std::string currentPath)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      returnSelectionOnly_(returnSelectionOnly),
      currentPath_(std::move(currentPath)) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  fonts_.clear();
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  ttfFonts_ = TtfFontScanner::scan();
  fonts_.reserve(ttfFonts_.size());

  for (int i = 0; i < static_cast<int>(ttfFonts_.size()); ++i) {
    FontEntry entry;
    entry.name = basenameFromPath(ttfFonts_[i].path);
    entry.kind = FontEntry::Kind::Ttf;
    entry.settingIndex = static_cast<uint8_t>(i);
    entry.path = ttfFonts_[i].path;
    entry.fileSize = static_cast<uint32_t>(std::min<uint64_t>(ttfFonts_[i].fileSize, UINT32_MAX));
    fonts_.push_back(std::move(entry));
  }

  if (fonts_.empty()) {
    fonts_.push_back({"No TTF fonts found in /TTF", FontEntry::Kind::Unavailable, 0});
  }

  selectedIndex_ = 0;
  const std::string selectedPath = returnSelectionOnly_ ? currentPath_ : std::string(SETTINGS.readerTtfPath);
  if (!selectedPath.empty()) {
    for (int i = 0; i < static_cast<int>(fonts_.size()); ++i) {
      if (fonts_[i].kind == FontEntry::Kind::Ttf && fonts_[i].path == selectedPath) {
        selectedIndex_ = i;
        break;
      }
    }
  }
#else
  // Build combined font list: built-in + SD card fonts
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0) + 8);

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), FontEntry::Kind::Builtin, CrossPointSettings::NOTOSERIF});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, FontEntry::Kind::SdCpfont,
                        static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  // Find current selection
  selectedIndex_ = 0;
  if (SETTINGS.sdFontFamilyName[0] != '\0' && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        selectedIndex_ = CrossPointSettings::BUILTIN_FONT_COUNT + i;
        break;
      }
    }
  } else {
    selectedIndex_ = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  }
#endif

  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  const auto& font = fonts_[selectedIndex_];
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (returnSelectionOnly_) {
    if (font.kind != FontEntry::Kind::Ttf) return;
    setResult(FilePathResult{font.path});
    finish();
    return;
  }
#endif
  if (font.kind == FontEntry::Kind::Builtin) {
    SETTINGS.readerFontMode = CrossPointSettings::READER_FONT_CPFONT;
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else if (font.kind == FontEntry::Kind::SdCpfont && registry_) {
    SETTINGS.readerFontMode = CrossPointSettings::READER_FONT_CPFONT;
    int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    }
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  } else if (font.kind == FontEntry::Kind::Ttf) {
    SETTINGS.readerFontMode = CrossPointSettings::READER_FONT_TTF;
    strncpy(SETTINGS.readerTtfPath, font.path.c_str(), sizeof(SETTINGS.readerTtfPath) - 1);
    SETTINGS.readerTtfPath[sizeof(SETTINGS.readerTtfPath) - 1] = '\0';
    SETTINGS.readerTtfFileSize = font.fileSize;
    SETTINGS.readerTtfHash = 0;
#endif
  } else if (font.kind == FontEntry::Kind::Unavailable) {
    return;
  }
  finish();
}

bool FontSelectionActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
    return true;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect listBounds{screen.x, contentTop, screen.width, renderer.getScreenHeight() - contentTop};
  const int listRows = std::max(1, listBounds.height / metrics.listRowHeight);
  const int listSize = static_cast<int>(fonts_.size());
  const auto layout = TouchList::calculatePageLayout(selectedIndex_, listSize, listRows);
  const int visibleRow = TouchNavigator::tappedListIndex(mappedInput, listBounds, TouchList::visibleRowCount(layout),
                                                        0, metrics.listRowHeight, 0);
  if (visibleRow < 0) {
    return mappedInput.wasTapped();
  }
  if (TouchList::isPreviousPageRow(layout, visibleRow)) {
    selectedIndex_ = TouchList::calculatePageLayout(std::max(0, layout.start - 1), listSize, listRows).start;
    requestUpdate();
    return true;
  }
  if (TouchList::isNextPageRow(layout, visibleRow)) {
    selectedIndex_ = std::min(listSize - 1, layout.start + layout.itemCount);
    requestUpdate();
    return true;
  }

  const int itemIndex = TouchList::visibleRowToItemIndex(layout, visibleRow);
  if (itemIndex >= 0) {
    selectedIndex_ = itemIndex;
    handleSelection();
  }
  return true;
#endif
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, tr(STR_FONT_FAMILY));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_FAMILY));
#endif

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
#ifdef CROSSPOINT_BOARD_MURPHY_M4
      pageHeight - contentTop;
#else
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  int currentFontIndex = -1;
  const std::string selectedPath = returnSelectionOnly_ ? currentPath_ : std::string(SETTINGS.readerTtfPath);
  if (!selectedPath.empty()) {
    for (int i = 0; i < static_cast<int>(fonts_.size()); ++i) {
      if (fonts_[i].kind == FontEntry::Kind::Ttf && fonts_[i].path == selectedPath) {
        currentFontIndex = i;
        break;
      }
    }
  }
#else
  int currentFontIndex = 0;
  // Determine which font index is currently active (to mark as "Selected")
    if (SETTINGS.sdFontFamilyName[0] != '\0' && registry_) {
      const auto& families = registry_->getFamilies();
      for (int i = 0; i < static_cast<int>(families.size()); i++) {
        if (families[i].name == SETTINGS.sdFontFamilyName) {
          currentFontIndex = CrossPointSettings::BUILTIN_FONT_COUNT + i;
          break;
        }
      }
    } else {
      currentFontIndex = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
    }
#endif

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int listSize = static_cast<int>(fonts_.size());
  const int listRows = std::max(1, contentHeight / metrics.listRowHeight);
  const auto layout = TouchList::calculatePageLayout(selectedIndex_, listSize, listRows);
  const Rect listBounds{0, contentTop, pageWidth, contentHeight};
  const int visibleSelected = layout.previous + selectedIndex_ - layout.start;
  GUI.drawList(
      renderer, listBounds, TouchList::visibleRowCount(layout), visibleSelected,
      [this, layout](int visibleRow) {
        if (TouchList::isPreviousPageRow(layout, visibleRow)) return std::string(tr(STR_PREV_PAGE));
        if (TouchList::isNextPageRow(layout, visibleRow)) return std::string(tr(STR_NEXT_PAGE));
        return fonts_[TouchList::visibleRowToItemIndex(layout, visibleRow)].name;
      },
      nullptr, nullptr,
      [layout, currentFontIndex](int visibleRow) -> std::string {
        const int index = TouchList::visibleRowToItemIndex(layout, visibleRow);
        return index == currentFontIndex ? tr(STR_SELECTED) : "";
      },
      true);
  if (layout.previous) TouchUi::drawCenteredPagerRow(renderer, listBounds, 0, tr(STR_PREV_PAGE));
  if (layout.next) {
    TouchUi::drawCenteredPagerRow(renderer, listBounds, TouchList::visibleRowCount(layout) - 1, tr(STR_NEXT_PAGE));
  }
#else
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string { return index == currentFontIndex ? tr(STR_SELECTED) : ""; },
      true);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
