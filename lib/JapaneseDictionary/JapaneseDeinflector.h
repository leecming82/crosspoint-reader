#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jpdict {

struct DeinflectionCandidate {
  DeinflectionCandidate() = default;
  DeinflectionCandidate(const std::string& candidateTerm, uint8_t candidateDepth)
      : term(candidateTerm), depth(candidateDepth) {}

  std::string term;
  uint8_t depth = 0;
};

std::vector<DeinflectionCandidate> expandDeinflections(const std::string& input,
                                                       uint8_t maxDepth = 3,
                                                       size_t maxCandidates = 32);
bool hasImmediateDeinflectionCandidate(const std::string& input);

}  // namespace jpdict
