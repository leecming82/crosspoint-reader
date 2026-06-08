#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchNavigator.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

namespace {
void drawTouchActionRow(const GfxRenderer& renderer, const Rect rect, const char* label) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  renderer.drawRoundedRect(rect.x + metrics.contentSidePadding, rect.y + 4,
                           rect.width - metrics.contentSidePadding * 2, rect.height - 8, 1, 6, true);

  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, rect.y + (rect.height - lineHeight) / 2,
                    label, true, EpdFontFamily::BOLD);
}
}  // namespace

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (fname.length() >= 4 && fname.substr(fname.length() - 4) == ".bmp") {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

#ifdef CROSSPOINT_BOARD_MURPHY_M4
      drawTouchActionRow(renderer, sleepCoverButtonRect(), tr(STR_SET_SLEEP_COVER));
#else
      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
#ifndef CROSSPOINT_BOARD_MURPHY_M4
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
#ifndef CROSSPOINT_BOARD_MURPHY_M4
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  bool success = false;
  HalFile inFile, outFile;
  if (Storage.openFileForRead("BMP", filePath, inFile)) {
    if (Storage.openFileForWrite("BMP", "/sleep.bmp", outFile)) {
      char buffer[2048];
      int bytesRead;
      success = true;
      while ((bytesRead = inFile.read(buffer, sizeof(buffer))) > 0) {
        if (outFile.write(buffer, bytesRead) != bytesRead) {
          success = false;
          break;
        }
      }
      outFile.close();
    }
    inFile.close();
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::goBackToFileBrowser() { activityManager.goToFileBrowser(filePath); }

bool BmpViewerActivity::showPreviousImage() {
  if (siblingImages.size() <= 1 || currentImageIndex <= 0) {
    return false;
  }

  currentImageIndex--;
  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  if (dirPath.back() != '/') dirPath += "/";
  filePath = dirPath + siblingImages[currentImageIndex];
  onEnter();
  return true;
}

bool BmpViewerActivity::showNextImage() {
  if (siblingImages.size() <= 1 || currentImageIndex == -1 ||
      currentImageIndex >= static_cast<int>(siblingImages.size()) - 1) {
    return false;
  }

  currentImageIndex++;
  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  if (dirPath.back() != '/') dirPath += "/";
  filePath = dirPath + siblingImages[currentImageIndex];
  onEnter();
  return true;
}

Rect BmpViewerActivity::sleepCoverButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, renderer.getScreenHeight() - metrics.listRowHeight, renderer.getScreenWidth(), metrics.listRowHeight};
}

Rect BmpViewerActivity::backTouchRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, 0, renderer.getScreenWidth() / 3, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing};
}

bool BmpViewerActivity::handleTouch() {
  if (!mappedInput.wasTapped()) {
    return false;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, backTouchRect())) {
    goBackToFileBrowser();
    return true;
  }

  if (TouchNavigator::wasTappedIn(mappedInput, sleepCoverButtonRect())) {
    doSetSleepCover();
    return true;
  }

  const auto tap = mappedInput.lastTap();
  const int screenWidth = renderer.getScreenWidth();
  if (tap.x < screenWidth / 3) {
    showPreviousImage();
    return true;
  }
  if (tap.x >= (screenWidth * 2) / 3) {
    showNextImage();
    return true;
  }

  return true;
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (handleTouch()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBackToFileBrowser();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    showPreviousImage();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    showNextImage();
    return;
  }
}
