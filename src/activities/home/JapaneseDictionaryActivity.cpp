#include "JapaneseDictionaryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <RomajiKana.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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
constexpr size_t STAGED_RESULT_LIMIT = JapaneseDictionaryActivity::MAX_RESULTS;
constexpr size_t NORMAL_RESULT_LIMIT = 8;
constexpr size_t MAX_QUERY_CHARS = 32;

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

std::string hiraganaToKatakana(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p != '\0') {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp >= 0x3041 && cp <= 0x3096) {
      cp += 0x60;
    } else if (cp == '-') {
      cp = 0x30FC;
    }
    utf8AppendCodepoint(cp, out);
  }
  return out;
}

int32_t searchSequenceGroupId(const int32_t sequence) { return sequence < 0 ? -sequence : sequence; }

bool searchHasTermVariant(const std::string& terms, const std::string& term) {
  size_t start = 0;
  while (start <= terms.size()) {
    const size_t end = terms.find("・", start);
    const size_t actualEnd = end == std::string::npos ? terms.size() : end;
    if (terms.substr(start, actualEnd - start) == term) return true;
    if (end == std::string::npos) break;
    start = end + strlen("・");
  }
  return false;
}

void appendSearchTermVariant(JapaneseDictionaryMatch& match, const std::string& term) {
  if (term.empty() || searchHasTermVariant(match.terms, term)) return;
  if (!match.terms.empty()) match.terms += "・";
  match.terms += term;
  if (match.termCount < UINT8_MAX) ++match.termCount;
}

bool mergeSearchResult(JapaneseDictionaryMatch* matches, const size_t count, const JapaneseDictionaryMatch& candidate) {
  const int32_t candidateGroup = searchSequenceGroupId(candidate.sequence);
  for (size_t i = 0; i < count; ++i) {
    if (searchSequenceGroupId(matches[i].sequence) == candidateGroup && matches[i].reading == candidate.reading &&
        matches[i].definition == candidate.definition) {
      appendSearchTermVariant(matches[i], candidate.term);
      return true;
    }
  }
  return false;
}

size_t appendSearchResults(JapaneseDictionaryMatch* outMatches, size_t found, const size_t maxMatches,
                           const JapaneseDictionaryMatch* candidates, const size_t candidateCount) {
  if (outMatches == nullptr || candidates == nullptr) return found;
  for (size_t i = 0; i < candidateCount && found < maxMatches; ++i) {
    if (!mergeSearchResult(outMatches, found, candidates[i])) {
      outMatches[found++] = candidates[i];
    }
  }
  return found;
}

bool rankedBefore(const JapaneseDictionaryMatch& a, const JapaneseDictionaryMatch& b) {
  if (a.tier != b.tier) return a.tier < b.tier;
  if (a.score != b.score) return a.score > b.score;
  if (a.deinflectionDepth != b.deinflectionDepth) return a.deinflectionDepth < b.deinflectionDepth;
  if (a.flags != b.flags) return a.flags < b.flags;
  return a.term < b.term;
}

bool endsWith(const std::string& text, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  return text.size() > suffixLen && text.compare(text.size() - suffixLen, suffixLen, suffix) == 0;
}

bool hasLikelyInflectionEnding(const std::string& query) {
  static constexpr const char* ENDINGS[] = {"ました", "ません", "ましたら", "ませんでした", "なかった", "かった",
                                            "くない", "くて",   "ければ",   "れば",         "られる",   "れる",
                                            "せる",   "させる", "ない",     "ます",         "たい",     "た",
                                            "て",     "だ",     "で",       "ば",           "ぬ"};
  for (const char* ending : ENDINGS) {
    if (endsWith(query, ending)) return true;
  }
  return false;
}

size_t stagedSearchLimit(const size_t found) {
  constexpr size_t MERGE_SLACK = 2;
  if (found >= NORMAL_RESULT_LIMIT) return 0;
  return std::min(STAGED_RESULT_LIMIT, NORMAL_RESULT_LIMIT - found + MERGE_SLACK);
}

std::vector<size_t> utf8CharStarts(const std::string& text, const size_t maxChars) {
  std::vector<size_t> starts;
  starts.reserve(std::min(maxChars, text.size()) + 1);
  const auto* begin = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* p = begin;
  while (*p != '\0' && starts.size() < maxChars) {
    starts.push_back(static_cast<size_t>(p - begin));
    utf8NextCodepoint(&p);
  }
  starts.push_back(static_cast<size_t>(p - begin));
  return starts;
}

size_t lookupSegmentedExact(JapaneseDictionary& dictionary, const std::string& query, JapaneseDictionaryMatch* outMatches,
                            const size_t maxMatches) {
  if (!dictionary.isOpen() || query.empty() || outMatches == nullptr || maxMatches < 2) return 0;

  const std::vector<size_t> starts = utf8CharStarts(query, MAX_QUERY_CHARS + 1);
  if (starts.size() < 5) return 0;
  const size_t charCount = starts.size() - 1;

  std::vector<std::string> segments;
  segments.reserve(maxMatches);
  size_t charPos = 0;
  while (charPos < charCount && segments.size() < maxMatches) {
    bool matched = false;
    const size_t remaining = charCount - charPos;
    if (remaining < 2) return 0;

    for (size_t segmentChars = remaining; segmentChars >= 2; --segmentChars) {
      const std::string segment = query.substr(starts[charPos], starts[charPos + segmentChars] - starts[charPos]);
      if (dictionary.hasExact(segment)) {
        segments.push_back(segment);
        charPos += segmentChars;
        matched = true;
        break;
      }
      if (segmentChars == 2) break;
    }
    if (!matched) return 0;
  }

  if (segments.size() < 2 || charPos != charCount) return 0;

  size_t found = 0;
  for (size_t segmentIndex = 0; segmentIndex < segments.size() && found < maxMatches; ++segmentIndex) {
    const size_t remainingSegments = segments.size() - segmentIndex - 1;
    const size_t available = maxMatches - found;
    if (available <= remainingSegments) return 0;
    const size_t segmentLimit = available - remainingSegments;
    const size_t segmentFound = dictionary.lookupExact(segments[segmentIndex], outMatches + found, segmentLimit);
    if (segmentFound == 0) return 0;
    found += segmentFound;
  }

  return found >= segments.size() ? found : 0;
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
  exactCursor = JapaneseDictionaryExactCursor{};
  searched = false;
  selectedResult = 0;
  detailLineOffset = 0;
  viewMode = ViewMode::Editing;
}

void JapaneseDictionaryActivity::search() {
  finalizePendingRomaji();
  results.clear();
  exactCursor = JapaneseDictionaryExactCursor{};
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

  std::vector<JapaneseDictionaryMatch> matches(MAX_RESULTS);
  std::vector<JapaneseDictionaryMatch> staged(STAGED_RESULT_LIMIT);
  size_t found = 0;
  size_t stagedCount = 0;
  const std::string katakanaQuery = hiraganaToKatakana(query);
  const bool likelyInflected = hasLikelyInflectionEnding(query);
  const bool allowKatakanaFallback = !likelyInflected;

  if (dictionary.beginExactLookup(query, exactCursor)) {
    stagedCount = dictionary.lookupExactNext(exactCursor, staged.data(), NORMAL_RESULT_LIMIT);
  }
  found = appendSearchResults(matches.data(), found, matches.size(), staged.data(), stagedCount);

  if (allowKatakanaFallback && found < NORMAL_RESULT_LIMIT && katakanaQuery != query) {
    stagedCount = dictionary.lookupExact(katakanaQuery, staged.data(), stagedSearchLimit(found));
    found = appendSearchResults(matches.data(), found, matches.size(), staged.data(), stagedCount);
  }

  std::sort(matches.begin(), matches.begin() + found, rankedBefore);
  if (found > NORMAL_RESULT_LIMIT) found = NORMAL_RESULT_LIMIT;

  if (found < NORMAL_RESULT_LIMIT) {
    stagedCount = dictionary.lookupPrefix(query, staged.data(), stagedSearchLimit(found));
    found = appendSearchResults(matches.data(), found, NORMAL_RESULT_LIMIT, staged.data(), stagedCount);
  }

  if (allowKatakanaFallback && found < NORMAL_RESULT_LIMIT && katakanaQuery != query) {
    stagedCount = dictionary.lookupPrefix(katakanaQuery, staged.data(), stagedSearchLimit(found));
    found = appendSearchResults(matches.data(), found, NORMAL_RESULT_LIMIT, staged.data(), stagedCount);
  }

  if (found < NORMAL_RESULT_LIMIT && likelyInflected) {
    stagedCount = dictionary.lookupDeinflected(query, staged.data(), stagedSearchLimit(found));
    found = appendSearchResults(matches.data(), found, NORMAL_RESULT_LIMIT, staged.data(), stagedCount);
  }

  if (found == 0) {
    found = lookupSegmentedExact(dictionary, query, matches.data(), matches.size());
  }

  results.assign(matches.begin(), matches.begin() + found);
  detailLineOffset = 0;
  viewMode = ViewMode::Results;
}

bool JapaneseDictionaryActivity::hasMoreExactResults() const {
  return dictionaryOpen && exactCursor.active && !exactCursor.exhausted && results.size() < MAX_RESULTS;
}

int JapaneseDictionaryActivity::resultLayoutItemCount() const {
  const size_t virtualMoreRow = hasMoreExactResults() ? 1 : 0;
  return static_cast<int>(std::min(MAX_RESULTS, results.size() + virtualMoreRow));
}

bool JapaneseDictionaryActivity::appendNextExactResultPage() {
  if (!hasMoreExactResults()) return false;

  const size_t oldCount = results.size();
  std::vector<JapaneseDictionaryMatch> merged(MAX_RESULTS);
  for (size_t i = 0; i < oldCount && i < merged.size(); ++i) {
    merged[i] = results[i];
  }

  std::vector<JapaneseDictionaryMatch> staged(STAGED_RESULT_LIMIT);
  size_t found = oldCount;
  while (hasMoreExactResults() && found < MAX_RESULTS) {
    const size_t remaining = MAX_RESULTS - found;
    const size_t pageLimit = std::min(remaining, NORMAL_RESULT_LIMIT);
    const size_t stagedCount = dictionary.lookupExactNext(exactCursor, staged.data(), pageLimit);
    if (stagedCount == 0) break;

    found = appendSearchResults(merged.data(), found, merged.size(), staged.data(), stagedCount);
    if (found > oldCount) {
      results.assign(merged.begin(), merged.begin() + found);
      return true;
    }
  }

  return results.size() > oldCount;
}

std::vector<std::string> JapaneseDictionaryActivity::detailLines() const {
  std::vector<std::string> lines;
  if (results.empty() || selectedResult >= results.size()) return lines;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const ParsedDefinition parsed = parseDefinition(results[selectedResult].definition);
  if (!parsed.attributes.empty()) {
    const auto tagLines = renderer.wrappedText(UI_10_FONT_ID, parsed.attributes.c_str(), width, 2, EpdFontFamily::BOLD);
    lines.insert(lines.end(), tagLines.begin(), tagLines.end());
  }

  for (size_t i = 0; i < parsed.glosses.size(); ++i) {
    const std::string prefix = std::to_string(i + 1) + ". ";
    const auto glossLines = renderer.wrappedText(UI_10_FONT_ID, (prefix + parsed.glosses[i]).c_str(), width, 64);
    lines.insert(lines.end(), glossLines.begin(), glossLines.end());
  }
  return lines;
}

int JapaneseDictionaryActivity::detailLinesPerPage(const int lineOffset) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentTop = titleTop + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int totalLines = static_cast<int>(detailLines().size());
  const bool hasPrevious = lineOffset > 0;
  const int previousHeight = hasPrevious ? metrics.listRowHeight : 0;
  const int availableBeforeNext = contentBottom - contentTop - previousHeight;
  int pageLines = std::max(1, availableBeforeNext / lineHeight);
  if (lineOffset + pageLines < totalLines) {
    pageLines = std::max(1, (availableBeforeNext - metrics.listRowHeight) / lineHeight);
  }
  return pageLines;
}

int JapaneseDictionaryActivity::detailMaxLineOffset() const {
  const int totalLines = static_cast<int>(detailLines().size());
  return std::max(0, totalLines - detailLinesPerPage(std::max(0, totalLines - 1)));
}

void JapaneseDictionaryActivity::pageDetail(const int direction) {
  if (direction < 0) {
    detailLineOffset = std::max(0, detailLineOffset - detailLinesPerPage(std::max(0, detailLineOffset - 1)));
  } else if (direction > 0) {
    detailLineOffset = std::min(detailMaxLineOffset(), detailLineOffset + detailLinesPerPage(detailLineOffset));
  }
  requestUpdate();
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
      calculateResultPageLayout(static_cast<int>(selectedResult), resultLayoutItemCount(), resultListRect().height,
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
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int titleTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentTop = titleTop + renderer.getLineHeight(UI_12_FONT_ID) + 8;
    const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    if (detailLineOffset > 0 &&
        TouchNavigator::contains(Rect{metrics.contentSidePadding, contentTop,
                                      renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.listRowHeight},
                                 point)) {
      pageDetail(-1);
      return true;
    }
    if (detailLineOffset < detailMaxLineOffset() &&
        TouchNavigator::contains(Rect{metrics.contentSidePadding, contentBottom - metrics.listRowHeight,
                                      renderer.getScreenWidth() - metrics.contentSidePadding * 2, metrics.listRowHeight},
                                 point)) {
      pageDetail(1);
      return true;
    }
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
        calculateResultPageLayout(static_cast<int>(selectedResult), resultLayoutItemCount(), list.height, RESULT_ROW_HEIGHT,
                                  metrics.listRowHeight);
    const int visibleRows = resultVisibleRowCount(layout);
    for (int i = 0; i < visibleRows; ++i) {
      if (!TouchNavigator::contains(resultVisibleRowRect(list, layout, i, RESULT_ROW_HEIGHT, metrics.listRowHeight),
                                    point)) {
        continue;
      }

      if (isPreviousResultPageRow(layout, i)) {
        selectedResult = static_cast<size_t>(
            calculateResultPageLayout(std::max(0, layout.start - 1), resultLayoutItemCount(), list.height,
                                      RESULT_ROW_HEIGHT, metrics.listRowHeight)
                .start);
        requestUpdate();
        return true;
      }
      if (isNextResultPageRow(layout, i)) {
        if (layout.start + layout.itemCount >= static_cast<int>(results.size()) && appendNextExactResultPage()) {
          selectedResult = static_cast<size_t>(std::min(static_cast<int>(results.size()) - 1,
                                                        layout.start + layout.itemCount));
          requestUpdate();
          return true;
        }
        selectedResult = static_cast<size_t>(std::min(static_cast<int>(results.size()) - 1,
                                                      layout.start + layout.itemCount));
        requestUpdate();
        return true;
      }

      const int resultIndex = visibleRowToResultIndex(layout, i);
      if (resultIndex < 0) return true;
      if (resultIndex >= static_cast<int>(results.size())) {
        if (appendNextExactResultPage()) {
          selectedResult = static_cast<size_t>(std::min(resultIndex, static_cast<int>(results.size()) - 1));
          requestUpdate();
        }
        return true;
      }
      selectedResult = static_cast<size_t>(resultIndex);
      detailLineOffset = 0;
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

  if (viewMode == ViewMode::Detail && !results.empty()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      pageDetail(-1);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      pageDetail(1);
    }
    return;
  }

  if ((viewMode == ViewMode::Results || viewMode == ViewMode::Detail) && !results.empty()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedResult = ButtonNavigator::previousIndex(static_cast<int>(selectedResult), static_cast<int>(results.size()));
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (selectedResult + 1 >= results.size() && appendNextExactResultPage()) {
        selectedResult = std::min(selectedResult + 1, results.size() - 1);
      } else {
        selectedResult = ButtonNavigator::nextIndex(static_cast<int>(selectedResult), static_cast<int>(results.size()));
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      const int perPage = resultsPerPage();
      selectedResult = static_cast<size_t>(std::max(0, static_cast<int>(selectedResult) - perPage));
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      const int perPage = resultsPerPage();
      if (static_cast<int>(selectedResult) + perPage >= static_cast<int>(results.size())) {
        appendNextExactResultPage();
      }
      selectedResult = static_cast<size_t>(
          std::min(static_cast<int>(results.size()) - 1, static_cast<int>(selectedResult) + perPage));
      requestUpdate();
    } else if (viewMode == ViewMode::Results && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      detailLineOffset = 0;
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
      calculateResultPageLayout(static_cast<int>(selectedResult), resultLayoutItemCount(), list.height, RESULT_ROW_HEIGHT,
                                metrics.listRowHeight);
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
    if (resultIndex >= static_cast<int>(results.size())) {
      TouchUi::drawCenteredPagerRow(renderer, row, 0, tr(STR_NEXT_PAGE));
      continue;
    }
    const bool selected = static_cast<size_t>(resultIndex) == selectedResult;
    if (selected) {
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height - 4, 4, Color::LightGray);
    } else {
      renderer.drawLine(row.x, row.y + row.height - 5, row.x + row.width, row.y + row.height - 5, 1, true);
    }

    const auto& match = results[resultIndex];
    const std::string title = matchTitle(match);
    const std::string titleClipped = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), row.width - 12,
                                                            EpdFontFamily::BOLD);
    const auto glossLines = renderer.wrappedText(UI_10_FONT_ID, definitionPreview(match.definition).c_str(),
                                                 row.width - 12, 2);
    renderer.drawText(UI_12_FONT_ID, row.x + 6, row.y + 6, titleClipped.c_str(), true, EpdFontFamily::BOLD);
    if (!glossLines.empty()) {
      renderer.drawText(UI_10_FONT_ID, row.x + 6, row.y + 32, glossLines[0].c_str(), true);
    }
    if (glossLines.size() > 1) {
      renderer.drawText(UI_10_FONT_ID, row.x + 6, row.y + 52, glossLines[1].c_str(), true);
    }
  }
}

void JapaneseDictionaryActivity::drawDetail() {
  if (results.empty() || selectedResult >= results.size()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& match = results[selectedResult];
  const int x = metrics.contentSidePadding;
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  const int titleTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int y = titleTop;

  const std::string title = renderer.truncatedText(UI_12_FONT_ID, matchTitle(match).c_str(), width, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  const int contentTop = titleTop + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  y = contentTop;

  const std::vector<std::string> lines = detailLines();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  if (detailLineOffset > 0) {
    const Rect row{x, y, width, metrics.listRowHeight};
    TouchUi::drawCenteredPagerRow(renderer, row, 0, tr(STR_PREV_PAGE));
    y += metrics.listRowHeight;
  }

  const int pageLines = detailLinesPerPage(detailLineOffset);
  const int endLine = std::min(static_cast<int>(lines.size()), detailLineOffset + pageLines);
  for (int i = detailLineOffset; i < endLine && y + lineHeight <= contentBottom; ++i) {
    renderer.drawText(UI_10_FONT_ID, x, y, lines[i].c_str(), true);
    y += lineHeight;
  }

  if (detailLineOffset < detailMaxLineOffset()) {
    const Rect row{x, contentBottom - metrics.listRowHeight, width, metrics.listRowHeight};
    TouchUi::drawCenteredPagerRow(renderer, row, 0, tr(STR_NEXT_PAGE));
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
