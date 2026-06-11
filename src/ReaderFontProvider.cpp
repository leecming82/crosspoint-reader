#include "ReaderFontProvider.h"

#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include "TtfReaderMetrics.h"
#endif

namespace ReaderFontProviders {

ReaderFontProvider* providerForConfig(const ReaderFontConfig& config) {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (config.isTtf()) return &TTF_READER_METRICS;
#endif
  return nullptr;
}

}  // namespace ReaderFontProviders
