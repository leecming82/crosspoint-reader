#include "JapaneseDictionaryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <RomajiKana.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/TouchKeyboard.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
constexpr const char* TITLE = "Japanese Dictionary";
constexpr int RESULT_ROW_HEIGHT = 88;

struct DictKeyDef {
  char key;
};

struct ResultPageLayout {
  int start = 0;
  int itemCount = 0;
  bool previous = false;
  bool next = false;
};

constexpr DictKeyDef KEY_ROWS[JapaneseDictionaryActivity::ROWS][JapaneseDictionaryActivity::COLS] = {
    {{'q'}, {'w'}, {'e'}, {'r'}, {'t'}},
    {{'a'}, {'s'}, {'d'}, {'f'}, {'g'}},
    {{'z'}, {'x'}, {'c'}, {'v'}, {'b'}},
    {{'y'}, {'u'}, {'i'}, {'o'}, {'p'}},
    {{'h'}, {'j'}, {'k'}, {'l'}, {'-'}},
    {{'n'}, {'m'}, {'\''}, {'.'}, {','}},
};

constexpr DictKeyDef SYMBOL_ROWS[JapaneseDictionaryActivity::ROWS][JapaneseDictionaryActivity::COLS] = {
    {{'1'}, {'2'}, {'3'}, {'4'}, {'5'}},
    {{'6'}, {'7'}, {'8'}, {'9'}, {'0'}},
    {{'!'}, {'?'}, {'-'}, {'\''}, {'.'}},
    {{','}, {'/'}, {':'}, {'('}, {')'}},
    {{'@'}, {'#'}, {'&'}, {'+'}, {'='}},
    {{'_'}, {'['}, {']'}, {'~'}, {'`'}},
};

const char* bottomLabel(const int col, const bool symbolsMode) {
  switch (col) {
    case 0:
      return "Space";
    case 1:
      return "Del";
    case 2:
      return "Clear";
    case 3:
      return symbolsMode ? "abc" : "123";
    case 4:
      return "Search";
    default:
      return "";
  }
}

KeyboardKeyType bottomType(const int col) {
  switch (col) {
    case 0:
      return KeyboardKeyType::Space;
    case 1:
      return KeyboardKeyType::Del;
    case 3:
      return KeyboardKeyType::Mode;
    case 4:
      return KeyboardKeyType::Ok;
    default:
      return KeyboardKeyType::Mode;
  }
}

std::string firstLine(const std::string& text) {
  const size_t newline = text.find('\n');
  if (newline == std::string::npos) return text;
  return text.substr(0, newline);
}

bool isDefinitionAttribute(const std::string& item) {
  static constexpr const char* attrs[] = {
      "1-dan",         "1-dan (spec.)", "2-dan",        "4-dan",          "5-dan",
      "5-dan (irreg.)", "5-dan (spec.)", "abbr.",        "adjectival",     "adjective",
      "adverb",        "anatomy",       "archaic",      "aux-adj",        "aux-verb",
      "auxiliary",     "biology",       "botany",       "business",       "childish",
      "colloquial",    "common word",   "conjunction",  "copula",         "counter",
      "dated",         "derogatory",    "exp",          "familiar",       "feminine",
      "formal",        "grammar",       "historical",   "honorific",      "humble",
      "idiom",         "interjection",  "intransitive", "jocular",        "ku-adj",
      "kuru",          "linguistics",   "masculine",    "na-adj",         "nari",
      "net slang",     "no-adj",        "noun",         "nu-verb",        "numeric",
      "obsolete",      "particle",      "person",       "place",          "poetical",
      "polite",        "prefix",        "pronoun",      "proverb",        "quote",
      "rare",          "ri-verb",       "sensitive",    "shiku",          "slang",
      "su-verb",       "suffix",        "suru verb",    "taru",           "to-adverb",
      "trademark",     "transitive",    "unclass",      "usually kana",   "verb",
      "vulgar",        "wasei",         "yoji",         "zuru",
  };
  for (const char* attr : attrs) {
    if (item == attr) return true;
  }
  return false;
}

std::string trimCopy(std::string item) {
  while (!item.empty() && item.front() == ' ') item.erase(item.begin());
  while (!item.empty() && item.back() == ' ') item.pop_back();
  return item;
}

std::vector<std::string> definitionItems(const std::string& definition) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start < definition.size()) {
    const size_t end = definition.find("; ", start);
    const std::string item = trimCopy(definition.substr(start, end == std::string::npos ? std::string::npos
                                                                                        : end - start));
    if (!item.empty()) items.push_back(item);
    if (end == std::string::npos) break;
    start = end + 2;
  }
  return items;
}

struct ParsedDefinition {
  std::string attributes;
  std::vector<std::string> glosses;
};

ParsedDefinition parseDefinition(const std::string& definition) {
  ParsedDefinition parsed;
  for (const auto& item : definitionItems(definition)) {
    if (isDefinitionAttribute(item)) {
      if (!parsed.attributes.empty()) parsed.attributes += ", ";
      parsed.attributes += item;
    } else {
      parsed.glosses.push_back(item);
    }
  }
  if (parsed.glosses.empty() && !definition.empty()) {
    parsed.glosses.push_back(firstLine(definition));
  }
  return parsed;
}

std::string definitionPreview(const std::string& definition) {
  std::string preview;
  for (const auto& item : parseDefinition(definition).glosses) {
    if (!preview.empty()) preview += "; ";
    preview += item;
    if (preview.size() > 96) break;
  }
  return preview.empty() ? firstLine(definition) : preview;
}

std::string matchTitle(const JapaneseDictionaryMatch& match) {
  std::string title = match.term;
  if (!match.reading.empty() && match.reading != match.term) {
    title += " [" + match.reading + "]";
  }
  return title;
}

ResultPageLayout calculateResultPageLayout(int selectedIndex, int itemCount, int listHeight, const int resultRowHeight,
                                           const int pagerRowHeight) {
  itemCount = std::max(0, itemCount);
  if (itemCount == 0) return {};

  listHeight = std::max(resultRowHeight, listHeight);
  selectedIndex = std::clamp(selectedIndex, 0, itemCount - 1);

  ResultPageLayout layout;
  int start = 0;
  bool previous = false;
  while (start < itemCount) {
    const int remaining = itemCount - start;
    const int previousHeight = previous ? pagerRowHeight : 0;
    const int heightBeforeNextPager = std::max(resultRowHeight, listHeight - previousHeight);
    const bool next = remaining * resultRowHeight > heightBeforeNextPager;
    const int itemHeight = std::max(resultRowHeight, heightBeforeNextPager - (next ? pagerRowHeight : 0));
    const int itemsOnPage = std::min(remaining, std::max(1, itemHeight / resultRowHeight));

    layout = ResultPageLayout{start, itemsOnPage, previous, next};
    if (selectedIndex < start + itemsOnPage || !next) break;

    start += itemsOnPage;
    previous = true;
  }
  return layout;
}

int resultVisibleRowCount(const ResultPageLayout& layout) {
  return layout.itemCount + (layout.previous ? 1 : 0) + (layout.next ? 1 : 0);
}

bool isPreviousResultPageRow(const ResultPageLayout& layout, const int visibleRow) {
  return layout.previous && visibleRow == 0;
}

bool isNextResultPageRow(const ResultPageLayout& layout, const int visibleRow) {
  return layout.next && visibleRow == resultVisibleRowCount(layout) - 1;
}

int visibleRowToResultIndex(const ResultPageLayout& layout, const int visibleRow) {
  if (visibleRow < 0 || isPreviousResultPageRow(layout, visibleRow) || isNextResultPageRow(layout, visibleRow)) {
    return -1;
  }
  const int itemOffset = visibleRow - (layout.previous ? 1 : 0);
  return (itemOffset >= 0 && itemOffset < layout.itemCount) ? layout.start + itemOffset : -1;
}

Rect resultVisibleRowRect(const Rect list, const ResultPageLayout& layout, const int visibleRow, const int resultRowHeight,
                          const int pagerRowHeight) {
  int y = list.y;
  if (isPreviousResultPageRow(layout, visibleRow)) {
    return Rect{list.x, y, list.width, pagerRowHeight};
  }
  if (layout.previous) y += pagerRowHeight;

  const int itemOffset = visibleRow - (layout.previous ? 1 : 0);
  if (itemOffset >= 0 && itemOffset < layout.itemCount) {
    return Rect{list.x, y + itemOffset * resultRowHeight, list.width, resultRowHeight};
  }

  return Rect{list.x, y + layout.itemCount * resultRowHeight, list.width, pagerRowHeight};
}

void drawBodyLine(const GfxRenderer& renderer, const int y, const char* text, const bool bold = false) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const std::string clipped =
      renderer.truncatedText(UI_12_FONT_ID, text, width, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, clipped.c_str(), true,
                    bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}
}  // namespace

void JapaneseDictionaryActivity::onEnter() {
  Activity::onEnter();
  bundleStatus = JapaneseDictionary::validateDefaultBundle();
  committedKana.clear();
  pendingRomaji.clear();
  results.clear();
  searched = false;
  viewMode = ViewMode::Editing;
  dictionaryOpen = false;
  symbolsMode = false;
  selectedResult = 0;
  requestUpdate();
}

std::string JapaneseDictionaryActivity::queryText() const { return committedKana + pendingRomaji; }

void JapaneseDictionaryActivity::insertChar(const char ch) {
  const bool romajiChar = (ch >= 'a' && ch <= 'z') || ch == '\'' || ch == '-';
  if (!romajiChar) {
    finalizePendingRomaji();
    committedKana.push_back(ch);
    return;
  }

  pendingRomaji.push_back(ch);
  const jpdict::RomajiComposition composed = jpdict::composeRomaji(pendingRomaji, false);
  committedKana += composed.committed;
  pendingRomaji = composed.pending;
}

void JapaneseDictionaryActivity::finalizePendingRomaji() {
  if (pendingRomaji.empty()) return;
  const jpdict::RomajiComposition composed = jpdict::composeRomaji(pendingRomaji, true);
  committedKana += composed.committed;
  pendingRomaji = composed.pending;
}

void JapaneseDictionaryActivity::insertSpace() {
  finalizePendingRomaji();
  committedKana.push_back(' ');
}

void JapaneseDictionaryActivity::backspace() {
  if (!pendingRomaji.empty()) {
    pendingRomaji.pop_back();
    return;
  }
  utf8RemoveLastChar(committedKana);
}

void JapaneseDictionaryActivity::clearQuery() {
  committedKana.clear();
  pendingRomaji.clear();
  results.clear();
  searched = false;
  selectedResult = 0;
  viewMode = ViewMode::Editing;
}

void JapaneseDictionaryActivity::search() {
  finalizePendingRomaji();
  results.clear();
  selectedResult = 0;
  searched = true;

  const std::string query = queryText();
  if (query.empty() || !bundleStatus.complete) {
    viewMode = ViewMode::Editing;
    return;
  }

  if (!dictionaryOpen) {
    dictionaryOpen = dictionary.openDefault();
  }
  if (!dictionaryOpen) {
    bundleStatus = JapaneseDictionary::validateDefaultBundle();
    viewMode = ViewMode::Results;
    return;
  }

  std::array<JapaneseDictionaryMatch, MAX_RESULTS> matches;
  const size_t found = dictionary.lookupExactThenPrefix(query, matches.data(), matches.size(), 48);
  results.assign(matches.begin(), matches.begin() + found);
  viewMode = ViewMode::Results;
}

void JapaneseDictionaryActivity::handleBack() {
  if (viewMode == ViewMode::Detail) {
    viewMode = ViewMode::Results;
    requestUpdate();
    return;
  }
  if (viewMode == ViewMode::Results) {
    clearQuery();
    requestUpdate();
    return;
  }
  onGoHome(HomeMenuItem::JAPANESE_DICTIONARY);
}

int JapaneseDictionaryActivity::resultsPerPage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const ResultPageLayout layout =
      calculateResultPageLayout(static_cast<int>(selectedResult), static_cast<int>(results.size()), resultListRect().height,
                                RESULT_ROW_HEIGHT, metrics.listRowHeight);
  return std::max(1, layout.itemCount);
}

Rect JapaneseDictionaryActivity::queryRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{metrics.contentSidePadding, metrics.topPadding + metrics.headerHeight + 4,
              renderer.getScreenWidth() - metrics.contentSidePadding * 2, 44};
}

Rect JapaneseDictionaryActivity::resultListRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect q = queryRect();
  const int y = q.y + q.height + metrics.verticalSpacing;
  return Rect{metrics.contentSidePadding, y, renderer.getScreenWidth() - metrics.contentSidePadding * 2,
              renderer.getScreenHeight() - y - metrics.buttonHintsHeight};
}

bool JapaneseDictionaryActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    handleBack();
    return true;
  }

  if (!mappedInput.wasTapped()) return false;
  const auto point = mappedInput.lastTap();
  if (!bundleStatus.complete) return true;

  if (viewMode == ViewMode::Detail) {
    return true;
  }

  if (TouchNavigator::contains(queryRect(), point) && viewMode == ViewMode::Results) {
    viewMode = ViewMode::Editing;
    requestUpdate();
    return true;
  }

  if (viewMode == ViewMode::Results) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect list = resultListRect();
    const ResultPageLayout layout =
        calculateResultPageLayout(static_cast<int>(selectedResult), static_cast<int>(results.size()), list.height,
                                  RESULT_ROW_HEIGHT, metrics.listRowHeight);
    const int visibleRows = resultVisibleRowCount(layout);
    for (int i = 0; i < visibleRows; ++i) {
      if (!TouchNavigator::contains(resultVisibleRowRect(list, layout, i, RESULT_ROW_HEIGHT, metrics.listRowHeight),
                                    point)) {
        continue;
      }

      if (isPreviousResultPageRow(layout, i)) {
        selectedResult = static_cast<size_t>(
            calculateResultPageLayout(std::max(0, layout.start - 1), static_cast<int>(results.size()), list.height,
                                      RESULT_ROW_HEIGHT, metrics.listRowHeight)
                .start);
        requestUpdate();
        return true;
      }
      if (isNextResultPageRow(layout, i)) {
        selectedResult = static_cast<size_t>(std::min(static_cast<int>(results.size()) - 1,
                                                      layout.start + layout.itemCount));
        requestUpdate();
        return true;
      }

      const int resultIndex = visibleRowToResultIndex(layout, i);
      if (resultIndex < 0) return true;
      selectedResult = static_cast<size_t>(resultIndex);
      viewMode = ViewMode::Detail;
      requestUpdate();
      return true;
    }
    return true;
  }

  const TouchKeyboardLayout keyboard(renderer, ROWS, COLS, BOTTOM_KEY_COUNT, 8, false, 0, COLS);
  int hitRow = 0;
  int hitCol = 0;
  if (keyboard.hitContentKey(point.x, point.y, hitRow, hitCol)) {
    insertChar(symbolsMode ? SYMBOL_ROWS[hitRow][hitCol].key : KEY_ROWS[hitRow][hitCol].key);
    searched = false;
    requestUpdate();
    return true;
  }

  if (keyboard.hitBottomKey(point.x, point.y, hitCol)) {
    switch (hitCol) {
      case 0:
        insertSpace();
        searched = false;
        break;
      case 1:
        backspace();
        searched = false;
        break;
      case 2:
        clearQuery();
        break;
      case 3:
        symbolsMode = !symbolsMode;
        break;
      case 4:
        search();
        break;
      default:
        break;
    }
    requestUpdate();
    return true;
  }

  return true;
#endif
}

void JapaneseDictionaryActivity::loop() {
  if (handleTouch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBack();
    return;
  }

  if (!bundleStatus.complete) return;

  if ((viewMode == ViewMode::Results || viewMode == ViewMode::Detail) && !results.empty()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedResult = ButtonNavigator::previousIndex(static_cast<int>(selectedResult), static_cast<int>(results.size()));
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedResult = ButtonNavigator::nextIndex(static_cast<int>(selectedResult), static_cast<int>(results.size()));
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      const int perPage = resultsPerPage();
      selectedResult = static_cast<size_t>(std::max(0, static_cast<int>(selectedResult) - perPage));
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      const int perPage = resultsPerPage();
      selectedResult = static_cast<size_t>(
          std::min(static_cast<int>(results.size()) - 1, static_cast<int>(selectedResult) + perPage));
      requestUpdate();
    } else if (viewMode == ViewMode::Results && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      viewMode = ViewMode::Detail;
      requestUpdate();
    }
  }
}

void JapaneseDictionaryActivity::drawMissingState() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  if (!bundleStatus.storageReady) {
    drawBodyLine(renderer, y, "Storage is not ready.", true);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
    drawBodyLine(renderer, y, "Insert or mount the SD card, then reopen this screen.");
    return;
  }

  drawBodyLine(renderer, y, "Dictionary data is incomplete.", true);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  drawBodyLine(renderer, y, "Required path: /.crosspoint/dicts");
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int maxShown = std::min(static_cast<int>(bundleStatus.missingFiles.size()), 6);
  for (int i = 0; i < maxShown; ++i) {
    const std::string line = "Missing: " + bundleStatus.missingFiles[i];
    drawBodyLine(renderer, y, line.c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  }
  if (static_cast<int>(bundleStatus.missingFiles.size()) > maxShown) {
    const std::string more = "+" + std::to_string(bundleStatus.missingFiles.size() - maxShown) + " more missing";
    drawBodyLine(renderer, y, more.c_str());
  }
}

void JapaneseDictionaryActivity::drawQueryField() {
  const Rect rect = queryRect();
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, 4, true);
  const std::string query = queryText();
  const std::string display = query.empty() ? std::string("Search") : query;
  const std::string clipped =
      renderer.truncatedText(UI_12_FONT_ID, display.c_str(), rect.width - 20, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int textY = rect.y + (rect.height - lineHeight) / 2;
  renderer.drawText(UI_12_FONT_ID, rect.x + 10, textY, clipped.c_str(), true, EpdFontFamily::BOLD);
}

void JapaneseDictionaryActivity::drawResults() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect list = resultListRect();
  if (!searched) {
    renderer.drawText(UI_12_FONT_ID, list.x, list.y, "Type a query and tap Search.", true);
    return;
  }
  if (queryText().empty()) {
    renderer.drawText(UI_12_FONT_ID, list.x, list.y, "Type a query first.", true);
    return;
  }
  if (!dictionaryOpen && results.empty()) {
    renderer.drawText(UI_12_FONT_ID, list.x, list.y, "Dictionary could not be opened.", true);
    return;
  }
  if (results.empty()) {
    renderer.drawText(UI_12_FONT_ID, list.x, list.y, "No matches.", true);
    return;
  }

  const auto layout =
      calculateResultPageLayout(static_cast<int>(selectedResult), static_cast<int>(results.size()), list.height,
                                RESULT_ROW_HEIGHT, metrics.listRowHeight);
  const int visibleRows = resultVisibleRowCount(layout);
  for (int i = 0; i < visibleRows; ++i) {
    const Rect row = resultVisibleRowRect(list, layout, i, RESULT_ROW_HEIGHT, metrics.listRowHeight);
    if (isPreviousResultPageRow(layout, i)) {
      TouchUi::drawCenteredPagerRow(renderer, row, 0, tr(STR_PREV_PAGE));
      continue;
    }
    if (isNextResultPageRow(layout, i)) {
      TouchUi::drawCenteredPagerRow(renderer, row, 0, tr(STR_NEXT_PAGE));
      continue;
    }

    const int resultIndex = visibleRowToResultIndex(layout, i);
    if (resultIndex < 0) continue;
    const bool selected = static_cast<size_t>(resultIndex) == selectedResult;
    if (selected) {
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height - 4, 4, Color::Black);
    } else {
      renderer.drawLine(row.x, row.y + row.height - 5, row.x + row.width, row.y + row.height - 5, 1, true);
    }

    const auto& match = results[resultIndex];
    const std::string title = matchTitle(match);
    const std::string titleClipped = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), row.width - 12,
                                                            EpdFontFamily::BOLD);
    const auto glossLines = renderer.wrappedText(UI_10_FONT_ID, definitionPreview(match.definition).c_str(),
                                                 row.width - 12, 2);
    renderer.drawText(UI_12_FONT_ID, row.x + 6, row.y + 6, titleClipped.c_str(), !selected, EpdFontFamily::BOLD);
    if (!glossLines.empty()) {
      renderer.drawText(UI_10_FONT_ID, row.x + 6, row.y + 32, glossLines[0].c_str(), !selected);
    }
    if (glossLines.size() > 1) {
      renderer.drawText(UI_10_FONT_ID, row.x + 6, row.y + 52, glossLines[1].c_str(), !selected);
    }
  }
}

void JapaneseDictionaryActivity::drawDetail() {
  if (results.empty() || selectedResult >= results.size()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& match = results[selectedResult];
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const std::string title = renderer.truncatedText(UI_12_FONT_ID, matchTitle(match).c_str(), width, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 8;

  const ParsedDefinition parsed = parseDefinition(match.definition);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;

  if (!parsed.attributes.empty() && y + lineHeight < contentBottom) {
    const std::string tags = renderer.truncatedText(UI_10_FONT_ID, parsed.attributes.c_str(), width, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, x, y, tags.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing;
  }

  for (size_t i = 0; i < parsed.glosses.size() && y + lineHeight <= contentBottom; ++i) {
    const std::string prefix = std::to_string(i + 1) + ". ";
    const int remainingLines = std::max(1, (contentBottom - y) / lineHeight);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, (prefix + parsed.glosses[i]).c_str(), width, remainingLines);
    for (const auto& line : lines) {
      if (y + lineHeight > contentBottom) return;
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true);
      y += lineHeight;
    }
    y += 4;
  }
}

void JapaneseDictionaryActivity::drawKeyboard() {
  const TouchKeyboardLayout keyboard(renderer, ROWS, COLS, BOTTOM_KEY_COUNT, 8, false, 0, COLS);
  for (int row = 0; row < ROWS; ++row) {
    for (int col = 0; col < COLS; ++col) {
      const char label[] = {symbolsMode ? SYMBOL_ROWS[row][col].key : KEY_ROWS[row][col].key, '\0'};
      GUI.drawKeyboardKey(renderer, keyboard.contentKeyRect(row, col), label, false);
    }
  }

  for (int col = 0; col < BOTTOM_KEY_COUNT; ++col) {
    GUI.drawKeyboardKey(renderer, keyboard.bottomKeyRect(col), bottomLabel(col, symbolsMode), false, nullptr,
                        bottomType(col));
  }
}

void JapaneseDictionaryActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, TITLE);
#else
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, TITLE);
#endif

  if (!bundleStatus.complete) {
    drawMissingState();
  } else {
    if (viewMode == ViewMode::Detail) {
      drawDetail();
    } else if (viewMode == ViewMode::Editing) {
      drawQueryField();
      drawKeyboard();
    } else {
      drawQueryField();
      drawResults();
    }
  }

#ifndef CROSSPOINT_BOARD_MURPHY_M4
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();
}
