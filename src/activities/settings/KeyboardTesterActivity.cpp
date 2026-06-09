#include "KeyboardTesterActivity.h"

#include <RomajiKana.h>
#include <Utf8.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchKeyboard.h"
#include "util/TouchNavigator.h"
#include "util/TouchUi.h"

namespace {
struct TesterKeyDef {
  char key;
};

constexpr TesterKeyDef KEY_ROWS[KeyboardTesterActivity::ROWS][KeyboardTesterActivity::COLS] = {
    {{'1'}, {'2'}, {'3'}, {'4'}, {'5'}, {'6'}, {'7'}, {'8'}},
    {{'9'}, {'0'}, {'q'}, {'w'}, {'e'}, {'r'}, {'t'}, {'y'}},
    {{'u'}, {'i'}, {'o'}, {'p'}, {'a'}, {'s'}, {'d'}, {'f'}},
    {{'g'}, {'h'}, {'j'}, {'k'}, {'l'}, {'-'}, {'z'}, {'x'}},
    {{'c'}, {'v'}, {'b'}, {'n'}, {'m'}, {'\''}, {'.'}, {','}},
};

const char* bottomLabel(const int col) {
  switch (col) {
    case 0:
      return "Space";
    case 1:
      return "Del";
    case 2:
      return "Clear";
    default:
      return "";
  }
}

KeyboardKeyType bottomType(const int col) {
  switch (col) {
    case 0:
      return KeyboardKeyType::Space;
    case 1:
      return KeyboardKeyType::Del;
    default:
      return KeyboardKeyType::Mode;
  }
}

void drawModeButton(const GfxRenderer& renderer, const Rect rect, const char* label, const bool selected) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect button{rect.x + metrics.contentSidePadding / 2, rect.y + 4, rect.width - metrics.contentSidePadding,
                    rect.height - 8};
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  if (selected) {
    renderer.fillRoundedRect(button.x, button.y, button.width, button.height, 6, Color::Black);
  } else {
    renderer.drawRoundedRect(button.x, button.y, button.width, button.height, 1, 6, true);
  }
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, label, !selected,
                    EpdFontFamily::BOLD);
}
}  // namespace

void KeyboardTesterActivity::onEnter() {
  Activity::onEnter();
  mode = Mode::English;
  requestUpdate();
}

void KeyboardTesterActivity::insertChar(const char ch) {
  if (mode == Mode::English) {
    englishText.push_back(ch);
    return;
  }

  pendingRomaji.push_back(ch);
  const jpdict::RomajiComposition composed = jpdict::composeRomaji(pendingRomaji, false);
  committedKana += composed.committed;
  pendingRomaji = composed.pending;
}

void KeyboardTesterActivity::finalizePendingRomaji() {
  if (pendingRomaji.empty()) return;
  const jpdict::RomajiComposition composed = jpdict::composeRomaji(pendingRomaji, true);
  committedKana += composed.committed;
  pendingRomaji = composed.pending;
}

void KeyboardTesterActivity::insertSpace() {
  if (mode == Mode::English) {
    englishText.push_back(' ');
    return;
  }
  finalizePendingRomaji();
  committedKana.push_back(' ');
}

void KeyboardTesterActivity::backspace() {
  if (mode == Mode::English) {
    if (!englishText.empty()) englishText.pop_back();
    return;
  }

  if (!pendingRomaji.empty()) {
    pendingRomaji.pop_back();
    return;
  }
  utf8RemoveLastChar(committedKana);
}

void KeyboardTesterActivity::clearText() {
  englishText.clear();
  committedKana.clear();
  pendingRomaji.clear();
}

std::string KeyboardTesterActivity::displayText() const {
  if (mode == Mode::English) {
    return englishText;
  }
  return committedKana + pendingRomaji;
}

Rect KeyboardTesterActivity::modeEnglishRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;
  const int w = renderer.getScreenWidth() / 2 - metrics.contentSidePadding;
  return Rect{metrics.contentSidePadding, y, w, 42};
}

Rect KeyboardTesterActivity::modeRomajiRect() const {
  const Rect english = modeEnglishRect();
  return Rect{english.x + english.width + 8, english.y, english.width, english.height};
}

bool KeyboardTesterActivity::handleTouch() {
#ifndef CROSSPOINT_BOARD_MURPHY_M4
  return false;
#else
  if (TouchNavigator::wasTappedIn(mappedInput, TouchUi::headerBackTapRect(renderer))) {
    finish();
    return true;
  }

  if (!mappedInput.wasTapped()) return false;
  const auto point = mappedInput.lastTap();

  if (TouchNavigator::contains(modeEnglishRect(), point)) {
    mode = Mode::English;
    requestUpdate();
    return true;
  }
  if (TouchNavigator::contains(modeRomajiRect(), point)) {
    mode = Mode::Romaji;
    requestUpdate();
    return true;
  }

  const TouchKeyboardLayout keyboard(renderer, ROWS, COLS, 3, 8, false, 0, COLS);
  int hitRow = 0;
  int hitCol = 0;
  if (keyboard.hitContentKey(point.x, point.y, hitRow, hitCol)) {
    insertChar(KEY_ROWS[hitRow][hitCol].key);
    requestUpdate();
    return true;
  }

  if (keyboard.hitBottomKey(point.x, point.y, hitCol)) {
    switch (hitCol) {
      case 0:
        insertSpace();
        break;
      case 1:
        backspace();
        break;
      case 2:
        clearText();
        break;
    }
    requestUpdate();
    return true;
  }

  return true;
#endif
}

void KeyboardTesterActivity::loop() {
  if (handleTouch()) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void KeyboardTesterActivity::render(RenderLock&&) {
  renderer.clearScreen();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  TouchUi::drawHeaderWithBack(renderer, screen, "Keyboard Tester");
#else
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 "Keyboard Tester");
#endif

  drawModeButton(renderer, modeEnglishRect(), "English", mode == Mode::English);
  drawModeButton(renderer, modeRomajiRect(), "Romaji", mode == Mode::Romaji);

  const auto& layoutMetrics = UITheme::getInstance().getMetrics();
  const int displayY = modeEnglishRect().y + modeEnglishRect().height + 16;
  const int displayX = layoutMetrics.contentSidePadding;
  const int displayWidth = renderer.getScreenWidth() - layoutMetrics.contentSidePadding * 2;
  const std::string text = displayText();
  const std::string shown = renderer.truncatedText(UI_12_FONT_ID, text.empty() ? "(empty)" : text.c_str(), displayWidth);
  renderer.drawRect(displayX, displayY, displayWidth, 48, 1, true);
  renderer.drawText(UI_12_FONT_ID, displayX + 10, displayY + 16, shown.c_str(), true);

  if (mode == Mode::Romaji) {
    const std::string pending = "pending: " + (pendingRomaji.empty() ? std::string("(none)") : pendingRomaji);
    renderer.drawText(UI_10_FONT_ID, displayX, displayY + 58, pending.c_str(), true);
  }

  const TouchKeyboardLayout keyboard(renderer, ROWS, COLS, 3, 8, false, 0, COLS);
  for (int row = 0; row < ROWS; ++row) {
    for (int col = 0; col < COLS; ++col) {
      const char label[] = {KEY_ROWS[row][col].key, '\0'};
      GUI.drawKeyboardKey(renderer, keyboard.contentKeyRect(row, col), label, false);
    }
  }

  for (int col = 0; col < 3; ++col) {
    GUI.drawKeyboardKey(renderer, keyboard.bottomKeyRect(col), bottomLabel(col), false, nullptr, bottomType(col));
  }

  renderer.displayBuffer();
}
