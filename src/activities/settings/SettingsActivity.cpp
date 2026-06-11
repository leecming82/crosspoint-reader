#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "HardwareDiagnosticsActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "ReaderFontSizeActivity.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/icons/back24.h"
#include "fontIds.h"

namespace {
constexpr int BACK_ICON_SIZE = 24;
constexpr int BACK_ICON_VISUAL_OFFSET_Y = 5;

struct SettingsPageLayout {
  int start = 0;
  int settingCount = 0;
  bool previous = false;
  bool next = false;
};

SettingsPageLayout calculateSettingsPageLayout(int selectedSetting, int settingsCount, int listRows) {
  settingsCount = std::max(0, settingsCount);
  listRows = std::max(1, listRows);
  selectedSetting = std::clamp(selectedSetting, 0, std::max(0, settingsCount - 1));

  SettingsPageLayout layout;
  int start = 0;
  bool previous = false;

  while (start < settingsCount) {
    const int remaining = settingsCount - start;
    const int rowsAvailable = std::max(1, listRows - (previous ? 1 : 0));
    const bool next = remaining > rowsAvailable;
    const int settingCountOnPage = next ? std::max(1, rowsAvailable - 1) : remaining;

    layout = SettingsPageLayout{start, settingCountOnPage, previous, next};
    if (selectedSetting < start + settingCountOnPage || !next) {
      break;
    }

    start += settingCountOnPage;
    previous = true;
  }

  return layout;
}

void drawSettingsHeaderTitle(const GfxRenderer& renderer, Rect backRect, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.drawIcon(Back24Icon, backRect.x + (backRect.width - BACK_ICON_SIZE) / 2,
                    backRect.y + (backRect.height - BACK_ICON_SIZE) / 2 + BACK_ICON_VISUAL_OFFSET_Y, BACK_ICON_SIZE,
                    BACK_ICON_SIZE);

  const int titleX = backRect.x + backRect.width + 8;
  const int titleMaxWidth = renderer.getScreenWidth() - titleX - metrics.contentSidePadding * 2 - metrics.batteryWidth;
  const auto headerTitle = renderer.truncatedText(UI_12_FONT_ID, title, titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, metrics.topPadding + metrics.batteryBarHeight + 3, headerTitle.c_str(), true,
                    EpdFontFamily::BOLD);
}

#ifdef CROSSPOINT_BOARD_MURPHY_M4
void drawCenteredPagerRow(const GfxRenderer& renderer, const Rect listBounds, const int visibleRow, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = metrics.listRowHeight;
  const int rowX = listBounds.x + metrics.contentSidePadding;
  const int rowWidth = listBounds.width - metrics.contentSidePadding * 2 - 1;
  const int rowY = listBounds.y + visibleRow * rowHeight;
  const Rect rowRect{rowX, rowY + 2, rowWidth, rowHeight - 4};

  renderer.fillRect(rowRect.x + 1, rowRect.y + 1, rowRect.width - 2, rowRect.height - 2, false);
  renderer.drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 1, 6, true);

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rowRect.y + (rowRect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rowRect.x + (rowRect.width - textWidth) / 2, textY, label, true,
                    EpdFontFamily::BOLD);
}
#endif
}  // namespace

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
#endif
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  systemSettings.push_back(SettingInfo::Action(StrId::STR_HARDWARE_DIAGNOSTICS, SettingAction::HardwareDiagnostics));
#endif
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

Rect SettingsActivity::backButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{8, metrics.topPadding + metrics.batteryBarHeight - 10, 48, 48};
}

Rect SettingsActivity::headerBackTapRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, 0, renderer.getScreenWidth() / 3, metrics.topPadding + metrics.headerHeight};
}

Rect SettingsActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight};
}

Rect SettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int height = renderer.getScreenHeight() - y - metrics.verticalSpacing;
#else
  const int height = renderer.getScreenHeight() - y - metrics.buttonHintsHeight - metrics.verticalSpacing;
#endif
  return Rect{0, y, renderer.getScreenWidth(), height};
}

int SettingsActivity::listPageItems() const {
  return std::max(1, listRect().height / UITheme::getInstance().getMetrics().listRowHeight);
}

int SettingsActivity::settingPageItems() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return calculateSettingsPageLayout(std::max(0, selectedSettingIndex - 1), settingsCount, listPageItems()).settingCount;
#else
  return listPageItems();
#endif
}

int SettingsActivity::currentListPageStart() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return calculateSettingsPageLayout(std::max(0, selectedSettingIndex - 1), settingsCount, listPageItems()).start;
#else
  const int selectedListIndex = std::max(0, selectedSettingIndex - 1);
  return (selectedListIndex / settingPageItems()) * settingPageItems();
#endif
}

bool SettingsActivity::hasPreviousListPage() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return calculateSettingsPageLayout(std::max(0, selectedSettingIndex - 1), settingsCount, listPageItems()).previous;
#else
  return currentListPageStart() > 0;
#endif
}

bool SettingsActivity::hasNextListPage() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return calculateSettingsPageLayout(std::max(0, selectedSettingIndex - 1), settingsCount, listPageItems()).next;
#else
  return currentListPageStart() + settingPageItems() < settingsCount;
#endif
}

int SettingsActivity::visibleSettingsCount() const {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return calculateSettingsPageLayout(std::max(0, selectedSettingIndex - 1), settingsCount, listPageItems()).settingCount;
#else
  return std::max(0, std::min(settingPageItems(), settingsCount - currentListPageStart()));
#endif
}

int SettingsActivity::visibleListRowCount() const {
  return visibleSettingsCount() + (hasPreviousListPage() ? 1 : 0) + (hasNextListPage() ? 1 : 0);
}

bool SettingsActivity::isPreviousPageRow(const int visibleRow) const {
  return hasPreviousListPage() && visibleRow == 0;
}

bool SettingsActivity::isNextPageRow(const int visibleRow) const {
  return hasNextListPage() && visibleRow == visibleListRowCount() - 1;
}

int SettingsActivity::visibleRowToSettingIndex(const int visibleRow) const {
  if (visibleRow < 0 || isPreviousPageRow(visibleRow) || isNextPageRow(visibleRow)) {
    return -1;
  }

  const int settingOffset = visibleRow - (hasPreviousListPage() ? 1 : 0);
  if (settingOffset < 0 || settingOffset >= visibleSettingsCount()) {
    return -1;
  }
  return currentListPageStart() + settingOffset;
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = std::clamp(categoryIndex, 0, categoryCount - 1);
  selectedSettingIndex = 1;
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

bool SettingsActivity::handleTouch() {
  if (!mappedInput.wasTapped()) {
    return false;
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (TouchNavigator::wasTappedIn(mappedInput, headerBackTapRect())) {
    SETTINGS.saveToFile();
    onGoHome();
    return true;
  }

  const int tabIndex = TouchNavigator::tappedEqualTabIndex(mappedInput, tabBarRect(), categoryCount);
  if (tabIndex >= 0) {
    selectCategory(tabIndex);
    requestUpdate();
    return true;
  }

  const int visibleRow = TouchNavigator::tappedListIndex(mappedInput, listRect(), visibleListRowCount(), 0,
                                                        UITheme::getInstance().getMetrics().listRowHeight, 0);
  if (visibleRow >= 0) {
    if (isPreviousPageRow(visibleRow)) {
      const auto previousPage =
          calculateSettingsPageLayout(std::max(0, currentListPageStart() - 1), settingsCount, listPageItems());
      selectedSettingIndex = previousPage.start + 1;
      requestUpdate();
      return true;
    }

    if (isNextPageRow(visibleRow)) {
      selectedSettingIndex = std::min(settingsCount, currentListPageStart() + visibleSettingsCount() + 1);
      requestUpdate();
      return true;
    }

    const int settingIndex = visibleRowToSettingIndex(visibleRow);
    if (settingIndex >= 0) {
      selectedSettingIndex = settingIndex + 1;
      toggleCurrentSetting();
      requestUpdate();
      return true;
    }
  }

  return false;
#endif

  const int tappedIndex = TouchNavigator::tappedListIndex(mappedInput, listRect(), settingsCount,
                                                          std::max(0, selectedSettingIndex - 1),
                                                          UITheme::getInstance().getMetrics().listRowHeight, 0);
  if (tappedIndex >= 0) {
    selectedSettingIndex = tappedIndex + 1;
    toggleCurrentSetting();
    requestUpdate();
    return true;
  }

  return false;
}

void SettingsActivity::loop() {
  bool hasChangedCategory = false;

  if (handleTouch()) {
    return;
  }

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const uint8_t previousFontSize = SETTINGS.fontSize;
  const uint8_t previousJapaneseFontSize = SETTINGS.japaneseFontSize;
  const uint8_t previousTtfFontSize = SETTINGS.readerTtfSizePx;
  const uint8_t previousTtfWeight = SETTINGS.readerTtfWeight;
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
  if (setting.valuePtr == &CrossPointSettings::readerTtfSizePx || setting.nameId == StrId::STR_FONT_WEIGHT) {
    openReaderFontSizePicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, nullptr),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::HardwareDiagnostics:
#ifdef CROSSPOINT_BOARD_MURPHY_M4
        startActivityForResult(std::make_unique<HardwareDiagnosticsActivity>(renderer, mappedInput), resultHandler);
#endif
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  CrossPointSettings::normalizeDependentSettings(SETTINGS);
  if (SETTINGS.fontSize != previousFontSize || SETTINGS.japaneseFontSize != previousJapaneseFontSize ||
      SETTINGS.readerTtfSizePx != previousTtfFontSize || SETTINGS.readerTtfWeight != previousTtfWeight) {
    SETTINGS.resetRubyOffsets();
  }
  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, StrId::STR_SLEEP_TIMER_STEP_HINT,
          SETTINGS.sleepTimeoutMinutes, CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
          CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true,
          StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::openReaderFontSizePicker() {
  const uint8_t previousTtfFontSize = SETTINGS.readerTtfSizePx;
  const uint8_t previousTtfWeight = SETTINGS.readerTtfWeight;
  startActivityForResult(std::make_unique<ReaderFontSizeActivity>(renderer, mappedInput),
                         [this, previousTtfFontSize, previousTtfWeight](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             if (const auto* selected = std::get_if<ReaderFontSettingsResult>(&result.data)) {
                               SETTINGS.readerTtfSizePx = selected->sizePx;
                               SETTINGS.readerTtfWeight = static_cast<uint8_t>(std::clamp<uint16_t>(
                                   static_cast<uint16_t>(selected->weight / 10), 10, 90));
                             }
                             if (SETTINGS.readerTtfSizePx != previousTtfFontSize ||
                                 SETTINGS.readerTtfWeight != previousTtfWeight) {
                               SETTINGS.resetRubyOffsets();
                             }
                             SETTINGS.saveToFile();
                           }
                           requestUpdate();
                         });
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  const auto& metrics = UITheme::getInstance().getMetrics();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");
  drawSettingsHeaderTitle(renderer, backButtonRect(), tr(STR_SETTINGS_TITLE));
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);
#endif

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, tabBarRect(), tabs, selectedSettingIndex == 0);

  const auto& settings = *currentSettings;

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const int visibleSelectedSetting =
      selectedSettingIndex > 0 ? (hasPreviousListPage() ? 1 : 0) + (selectedSettingIndex - 1 - currentListPageStart()) : -1;
  GUI.drawList(
      renderer, listRect(), visibleListRowCount(), visibleSelectedSetting,
      [this, &settings](int visibleRow) {
        if (isPreviousPageRow(visibleRow)) {
          return std::string(tr(STR_PREV_PAGE));
        }
        if (isNextPageRow(visibleRow)) {
          return std::string(tr(STR_NEXT_PAGE));
        }

        const int settingIndex = visibleRowToSettingIndex(visibleRow);
        if (settingIndex >= 0) {
          return std::string(I18N.get(settings[settingIndex].nameId));
        }
        return std::string();
      },
      nullptr, nullptr,
      [this, &settings](int visibleRow) {
        const int settingIndex = visibleRowToSettingIndex(visibleRow);
        if (settingIndex < 0) {
          return std::string();
        }

        const auto& setting = settings[settingIndex];
        std::string valueText;
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        }
        return valueText;
      },
      true);

  const Rect settingsListRect = listRect();
  if (hasPreviousListPage()) {
    drawCenteredPagerRow(renderer, settingsListRect, 0, tr(STR_PREV_PAGE));
  }
  if (hasNextListPage()) {
    drawCenteredPagerRow(renderer, settingsListRect, visibleListRowCount() - 1, tr(STR_NEXT_PAGE));
  }
#else
  GUI.drawList(
      renderer, listRect(),
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel =
      (selectedSettingIndex == 0)
          ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
          : (selectedSettingIndex > 0 && (*currentSettings)[selectedSettingIndex - 1].nameId == StrId::STR_TIME_TO_SLEEP
                 ? tr(STR_SELECT)
                 : tr(STR_TOGGLE));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
