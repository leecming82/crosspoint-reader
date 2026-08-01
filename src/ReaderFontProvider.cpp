#include "ReaderFontProvider.h"

#ifdef CROSSPOINT_TTF_READER_DIRECT_FREETYPE
#include "TtfReaderMetrics.h"
#endif

namespace ReaderFontProviders {

ReaderFontProvider* providerForConfig(const ReaderFontConfig& config) {
#ifdef CROSSPOINT_TTF_READER_DIRECT_FREETYPE
  if (config.isTtf()) return &TTF_READER_METRICS;
#endif
  return nullptr;
}

}  // namespace ReaderFontProviders
