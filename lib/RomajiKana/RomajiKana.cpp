#include "RomajiKana.h"

#include <cctype>
#include <cstring>

namespace jpdict {
namespace {

struct KanaRule {
  const char* romaji;
  const char* hiragana;
};

constexpr KanaRule kRules[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"ja", "じゃ"},  {"ju", "じゅ"},  {"jo", "じょ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"fa", "ふぁ"},  {"fi", "ふぃ"},  {"fe", "ふぇ"},  {"fo", "ふぉ"},
    {"tsa", "つぁ"}, {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"},
    {"she", "しぇ"}, {"che", "ちぇ"}, {"je", "じぇ"},
    {"wi", "うぃ"},  {"we", "うぇ"},  {"va", "ゔぁ"}, {"vi", "ゔぃ"},
    {"vu", "ゔ"},    {"ve", "ゔぇ"}, {"vo", "ゔぉ"},

    {"shi", "し"}, {"chi", "ち"}, {"tsu", "つ"}, {"fu", "ふ"},
    {"ji", "じ"},  {"di", "ぢ"},  {"du", "づ"},

    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"sa", "さ"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"za", "ざ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"ta", "た"}, {"ti", "ち"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"da", "だ"}, {"de", "で"}, {"do", "ど"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"},

    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
};

bool isAsciiVowel(const char ch) {
  return ch == 'a' || ch == 'i' || ch == 'u' || ch == 'e' || ch == 'o';
}

bool isAsciiConsonant(const char ch) { return ch >= 'a' && ch <= 'z' && !isAsciiVowel(ch); }

std::string lowerAscii(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (const unsigned char ch : input) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool startsWithAt(const std::string& text, const size_t pos, const char* prefix) {
  const size_t len = std::strlen(prefix);
  return pos + len <= text.size() && text.compare(pos, len, prefix) == 0;
}

const KanaRule* matchRule(const std::string& text, const size_t pos) {
  for (const auto& rule : kRules) {
    if (startsWithAt(text, pos, rule.romaji)) {
      return &rule;
    }
  }
  return nullptr;
}

bool isRulePrefix(const std::string& text, const size_t pos) {
  for (const auto& rule : kRules) {
    const size_t remaining = text.size() - pos;
    const size_t ruleLen = std::strlen(rule.romaji);
    if (remaining < ruleLen && std::strncmp(text.c_str() + pos, rule.romaji, remaining) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

RomajiComposition composeRomaji(const std::string& input, const bool final) {
  const std::string text = lowerAscii(input);
  RomajiComposition composed;
  composed.committed.reserve(text.size() * 3);

  size_t i = 0;
  for (; i < text.size();) {
    const char ch = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';

    if (ch == '-') {
      composed.committed += "ー";
      ++i;
      continue;
    }

    if (isAsciiConsonant(ch) && ch != 'n' && ch == next) {
      composed.committed += "っ";
      ++i;
      continue;
    }

    if (ch == 't' && next == 'c') {
      composed.committed += "っ";
      ++i;
      continue;
    }

    if (ch == 'n') {
      const char afterNext = i + 2 < text.size() ? text[i + 2] : '\0';
      if (next == '\'') {
        composed.committed += "ん";
        i += 2;
        continue;
      }
      if (next == '\0') {
        if (final) {
          composed.committed += "ん";
          ++i;
          continue;
        }
        break;
      }
      if (next == 'n' && afterNext == '\0') {
        composed.committed += "ん";
        i += 2;
        continue;
      }
      if (next == 'n') {
        composed.committed += "ん";
        ++i;
        continue;
      }
      if (!isAsciiVowel(next) && next != 'y') {
        composed.committed += "ん";
        ++i;
        continue;
      }
    }

    if (const KanaRule* rule = matchRule(text, i)) {
      composed.committed += rule->hiragana;
      i += std::strlen(rule->romaji);
      continue;
    }

    if (!final && isRulePrefix(text, i)) {
      break;
    }

    composed.committed.push_back(input[i]);
    ++i;
  }

  composed.pending = text.substr(i);
  return composed;
}

std::string romajiToHiragana(const std::string& input) { return composeRomaji(input, true).committed; }

}  // namespace jpdict
