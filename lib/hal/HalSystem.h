#pragma once

#include <cstdint>
#include <string>

#include "BoardProfile.h"

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

void logBootDiagnostics(const BoardCapabilityProfile& board);
void logStorageDiagnostics(bool storageReady);

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
}  // namespace HalSystem
