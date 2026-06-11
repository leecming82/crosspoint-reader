#pragma once

#include <SdCardFontRegistry.h>
#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include "TtfFontScanner.h"
#endif

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FontSelectionActivity final : public Activity {
 public:
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry);
  FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                        bool returnSelectionOnly, std::string currentPath = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  bool handleTouch();

  struct FontEntry {
    enum class Kind : uint8_t {
      Builtin,
      SdCpfont,
      Ttf,
      Unavailable,
    };

    std::string name;
    Kind kind = Kind::Builtin;
    uint8_t settingIndex = 0;
    std::string path;
    uint32_t fileSize = 0;
  };

  const SdCardFontRegistry* registry_;
  bool returnSelectionOnly_ = false;
  std::string currentPath_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  std::vector<TtfFontCandidate> ttfFonts_;
#endif
  int selectedIndex_ = 0;
};
