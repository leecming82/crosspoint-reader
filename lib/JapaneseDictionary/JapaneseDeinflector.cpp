#include "JapaneseDeinflector.h"

#include <algorithm>
#include <cstring>

namespace jpdict {
namespace {

bool endsWith(const std::string& word, const char* suffix) {
  const size_t suffixLen = std::strlen(suffix);
  return word.size() >= suffixLen &&
         word.compare(word.size() - suffixLen, suffixLen, suffix) == 0;
}

std::string replaceSuffix(const std::string& word, const char* suffix,
                          const char* replacement) {
  return word.substr(0, word.size() - std::strlen(suffix)) + replacement;
}

std::string utf8LastChar(const std::string& text) {
  if (text.empty()) {
    return "";
  }
  size_t pos = text.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  return text.substr(pos);
}

bool containsString(const std::vector<std::string>& values,
                    const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
  if (!value.empty() && !containsString(values, value)) {
    values.push_back(value);
  }
}

void addSuffixCandidate(std::vector<std::string>& out, const std::string& word,
                        const char* suffix, const char* replacement) {
  if (endsWith(word, suffix)) {
    addUnique(out, replaceSuffix(word, suffix, replacement));
  }
}

void addTrimmedAuxiliaryTailCandidates(std::vector<std::string>& out,
                                       const std::string& word) {
  static constexpr const char* suffixes[] = {
      "だけだった",   "だけでした",   "だけだ",   "だけです",   "だけ",
      "ばかりだった", "ばかりでした", "ばかりだ", "ばかりです", "ばかり",
  };
  for (const char* suffix : suffixes) {
    if (endsWith(word, suffix)) {
      addUnique(out, word.substr(0, word.size() - std::strlen(suffix)));
    }
  }
}

bool isGodanARow(const std::string& ch) {
  static constexpr const char* values[] = {"わ", "か", "が", "さ", "ざ",
                                           "た", "だ", "な", "は", "ば",
                                           "ぱ", "ま", "ら"};
  for (const char* value : values) {
    if (ch == value) {
      return true;
    }
  }
  return false;
}

const char* godanFromARow(const std::string& ch) {
  struct Pair {
    const char* from;
    const char* to;
  };
  static constexpr Pair pairs[] = {
      {"わ", "う"}, {"か", "く"}, {"が", "ぐ"}, {"さ", "す"},
      {"ざ", "ず"}, {"た", "つ"}, {"だ", "づ"}, {"な", "ぬ"},
      {"は", "ふ"}, {"ば", "ぶ"}, {"ぱ", "ぷ"}, {"ま", "む"},
      {"ら", "る"}};
  for (const auto& pair : pairs) {
    if (ch == pair.from) {
      return pair.to;
    }
  }
  return nullptr;
}

const char* godanFromMasuStem(const std::string& ch) {
  struct Pair {
    const char* from;
    const char* to;
  };
  static constexpr Pair pairs[] = {
      {"い", "う"}, {"き", "く"}, {"ぎ", "ぐ"}, {"し", "す"},
      {"じ", "ず"}, {"ち", "つ"}, {"ぢ", "づ"}, {"に", "ぬ"},
      {"ひ", "ふ"}, {"び", "ぶ"}, {"ぴ", "ぷ"}, {"み", "む"},
      {"り", "る"}};
  for (const auto& pair : pairs) {
    if (ch == pair.from) {
      return pair.to;
    }
  }
  return nullptr;
}

void addTeTaCandidates(std::vector<std::string>& out, const std::string& word) {
  static constexpr const char* uTsuru[] = {"う", "つ", "る"};
  for (const char* repl : uTsuru) {
    addSuffixCandidate(out, word, "って", repl);
    addSuffixCandidate(out, word, "った", repl);
  }
  static constexpr const char* bumnu[] = {"ぶ", "む", "ぬ"};
  for (const char* repl : bumnu) {
    addSuffixCandidate(out, word, "んで", repl);
    addSuffixCandidate(out, word, "んだ", repl);
  }
  addSuffixCandidate(out, word, "いて", "く");
  addSuffixCandidate(out, word, "いた", "く");
  addSuffixCandidate(out, word, "いで", "ぐ");
  addSuffixCandidate(out, word, "いだ", "ぐ");
  addSuffixCandidate(out, word, "して", "す");
  addSuffixCandidate(out, word, "した", "す");
  addSuffixCandidate(out, word, "して", "する");
  addSuffixCandidate(out, word, "した", "する");
  addSuffixCandidate(out, word, "行って", "行く");
  addSuffixCandidate(out, word, "行った", "行く");
  addSuffixCandidate(out, word, "来て", "来る");
  addSuffixCandidate(out, word, "来た", "来る");
  addSuffixCandidate(out, word, "來て", "來る");
  addSuffixCandidate(out, word, "來た", "來る");

  if (endsWith(word, "て") || endsWith(word, "た")) {
    const std::string stem =
        word.substr(0, word.size() - std::strlen(endsWith(word, "て") ? "て" : "た"));
    const std::string last = utf8LastChar(stem);
    if (last != "っ" && last != "ん" && last != "い") {
      addUnique(out, stem + "る");
    }
  }
}

void addPoliteStemCandidates(std::vector<std::string>& out,
                             const std::string& stem) {
  if (stem.empty()) {
    return;
  }
  addUnique(out, stem);
  const std::string last = utf8LastChar(stem);
  if (const char* repl = godanFromMasuStem(last)) {
    addUnique(out, stem.substr(0, stem.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, stem, "し", "する");
  addSuffixCandidate(out, stem, "こ", "くる");
  addSuffixCandidate(out, stem, "来", "来る");
  addSuffixCandidate(out, stem, "來", "來る");
  if (!isGodanARow(last)) {
    addUnique(out, stem + "る");
  }
}

void addBareMasuStemCandidates(std::vector<std::string>& out,
                               const std::string& word) {
  const std::string last = utf8LastChar(word);
  if (const char* repl = godanFromMasuStem(last)) {
    addUnique(out, word.substr(0, word.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, word, "し", "する");
  addSuffixCandidate(out, word, "こ", "くる");
  addSuffixCandidate(out, word, "来", "来る");
  addSuffixCandidate(out, word, "來", "來る");
  if (!isGodanARow(last)) {
    addUnique(out, word + "る");
  }
}

void addNegativeCandidates(std::vector<std::string>& out,
                           const std::string& word) {
  if (endsWith(word, "なかった")) {
    addUnique(out, replaceSuffix(word, "なかった", "ない"));
  }
  if (endsWith(word, "ませんでした")) {
    addUnique(out, replaceSuffix(word, "ませんでした", "ません"));
  }
  if (!endsWith(word, "ない")) {
    return;
  }
  const std::string stem = word.substr(0, word.size() - std::strlen("ない"));
  if (stem.empty()) {
    return;
  }
  const std::string last = utf8LastChar(stem);
  if (const char* repl = godanFromARow(last)) {
    addUnique(out, stem.substr(0, stem.size() - last.size()) + repl);
  }
  addSuffixCandidate(out, stem, "し", "する");
  addSuffixCandidate(out, stem, "こ", "くる");
  addSuffixCandidate(out, stem, "来", "来る");
  addSuffixCandidate(out, stem, "來", "來る");
  if (!isGodanARow(last)) {
    addUnique(out, stem + "る");
  }
}

void addIAdjectiveCandidates(std::vector<std::string>& out,
                             const std::string& word) {
  struct Rule {
    const char* suffix;
    const char* replacement;
  };
  static constexpr Rule rules[] = {
      {"く", "い"},
      {"くて", "い"},
      {"かった", "い"},
      {"かったです", "い"},
      {"くない", "い"},
      {"くないです", "い"},
      {"くなかった", "い"},
      {"くなかったです", "い"},
      {"くありません", "い"},
      {"くありませんでした", "い"},
  };
  for (const auto& rule : rules) {
    if (!endsWith(word, rule.suffix)) {
      continue;
    }
    const std::string stem = word.substr(0, word.size() - std::strlen(rule.suffix));
    if (stem == "い") {
      continue;
    }
    addUnique(out, stem + rule.replacement);
  }
}

void addInflectionStep(std::vector<std::string>& out, const std::string& word) {
  addTrimmedAuxiliaryTailCandidates(out, word);
  addTeTaCandidates(out, word);
  addBareMasuStemCandidates(out, word);
  addNegativeCandidates(out, word);
  addIAdjectiveCandidates(out, word);

  static constexpr const char* politeSuffixes[] = {
      "ませんでした", "ました", "ません", "ましょう", "ます"};
  for (const char* suffix : politeSuffixes) {
    if (endsWith(word, suffix)) {
      addPoliteStemCandidates(out,
                              word.substr(0, word.size() - std::strlen(suffix)));
    }
  }

  addSuffixCandidate(out, word, "ちゃう", "て");
  addSuffixCandidate(out, word, "じゃう", "で");
  addSuffixCandidate(out, word, "ちゃった", "て");
  addSuffixCandidate(out, word, "じゃった", "で");
  addSuffixCandidate(out, word, "ちゃって", "て");
  addSuffixCandidate(out, word, "じゃって", "で");
  addSuffixCandidate(out, word, "ている", "て");
  addSuffixCandidate(out, word, "でいる", "で");
  addSuffixCandidate(out, word, "ていた", "て");
  addSuffixCandidate(out, word, "でいた", "で");

  struct Rule {
    const char* suffix;
    const char* replacement;
  };
  static constexpr Rule rules[] = {
      {"えば", "う"},       {"けば", "く"},     {"げば", "ぐ"},
      {"せば", "す"},       {"てば", "つ"},     {"ねば", "ぬ"},
      {"べば", "ぶ"},       {"めば", "む"},     {"れば", "る"},
      {"ければ", "い"},     {"おう", "う"},     {"こう", "く"},
      {"ごう", "ぐ"},       {"そう", "す"},     {"とう", "つ"},
      {"のう", "ぬ"},       {"ぼう", "ぶ"},     {"もう", "む"},
      {"ろう", "る"},       {"よう", "る"},     {"われ", "う"},
      {"かれ", "く"},       {"がれ", "ぐ"},     {"され", "す"},
      {"たれ", "つ"},       {"なれ", "ぬ"},     {"ばれ", "ぶ"},
      {"まれ", "む"},       {"られ", "る"},     {"わせ", "う"},
      {"かせ", "く"},       {"がせ", "ぐ"},     {"させ", "す"},
      {"たせ", "つ"},       {"なせ", "ぬ"},     {"ばせ", "ぶ"},
      {"ませ", "む"},       {"らせ", "る"},     {"われる", "う"},
      {"かれる", "く"},     {"がれる", "ぐ"},   {"される", "す"},
      {"たれる", "つ"},     {"なれる", "ぬ"},   {"ばれる", "ぶ"},
      {"まれる", "む"},     {"られる", "る"},   {"わせる", "う"},
      {"かせる", "く"},     {"がせる", "ぐ"},   {"させる", "す"},
      {"たせる", "つ"},     {"なせる", "ぬ"},   {"ばせる", "ぶ"},
      {"ませる", "む"},     {"らせる", "る"},   {"ける", "く"},
      {"げる", "ぐ"},       {"せる", "す"},     {"てる", "つ"},
      {"ねる", "ぬ"},       {"べる", "ぶ"},     {"める", "む"},
      {"れる", "る"},       {"しよう", "する"}, {"こよう", "くる"},
      {"来よう", "来る"},   {"される", "する"}, {"され", "する"},
      {"こられる", "くる"}, {"来られる", "来る"}, {"こられ", "くる"},
      {"来られ", "来る"},   {"こさせる", "くる"}, {"来させる", "来る"},
      {"こさせ", "くる"},   {"来させ", "来る"}};
  for (const auto& rule : rules) {
    addSuffixCandidate(out, word, rule.suffix, rule.replacement);
  }
}

bool containsCandidateTerm(const std::vector<DeinflectionCandidate>& values,
                           const std::string& term) {
  return std::find_if(values.begin(), values.end(),
                      [&term](const DeinflectionCandidate& value) {
                        return value.term == term;
                      }) != values.end();
}

}  // namespace

std::vector<DeinflectionCandidate> expandDeinflections(
    const std::string& input, uint8_t maxDepth, size_t maxCandidates) {
  std::vector<DeinflectionCandidate> ordered;
  if (input.empty() || maxCandidates == 0) {
    return ordered;
  }
  ordered.push_back(DeinflectionCandidate(input, 0));
  size_t levelStart = 0;
  size_t levelEnd = ordered.size();

  for (uint8_t depth = 0;
       depth < maxDepth && levelStart < levelEnd && ordered.size() < maxCandidates;
       ++depth) {
    const size_t before = ordered.size();
    for (size_t i = levelStart; i < levelEnd && ordered.size() < maxCandidates;
         ++i) {
      std::vector<std::string> next;
      addInflectionStep(next, ordered[i].term);
      for (const auto& term : next) {
        if (ordered.size() >= maxCandidates) {
          break;
        }
        if (!containsCandidateTerm(ordered, term)) {
          ordered.push_back(
              DeinflectionCandidate(term, static_cast<uint8_t>(depth + 1)));
        }
      }
    }
    levelStart = levelEnd;
    levelEnd = ordered.size();
    if (before == levelEnd) {
      break;
    }
  }
  return ordered;
}

bool hasImmediateDeinflectionCandidate(const std::string& input) {
  std::vector<std::string> next;
  addInflectionStep(next, input);
  return !next.empty();
}

}  // namespace jpdict
