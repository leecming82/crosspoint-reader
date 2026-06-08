#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

struct Rect;

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  void goBackToFileBrowser();
  bool showPreviousImage();
  bool showNextImage();
  bool handleTouch();
  Rect sleepCoverButtonRect() const;
  Rect backTouchRect() const;

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};
