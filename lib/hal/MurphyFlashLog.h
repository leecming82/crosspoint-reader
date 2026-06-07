#pragma once

#include <stddef.h>

namespace MurphyFlashLog {

bool begin();
bool ready();
bool append(const char* line);
bool append(const char* data, size_t length);

}  // namespace MurphyFlashLog
