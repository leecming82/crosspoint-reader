#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalEnvSensor.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <HalTouch.h>
#include <I18n.h>
#include <Logging.h>
#include <MurphyFlashLog.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include "TtfReaderMetrics.h"
#endif
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/util/FrontlightOverlayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiLifecycle.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

MappedInputManager mappedInputManager(gpio, halTouch);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;

bool bootDiagnosticsOnly = false;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

namespace {

#ifdef CROSSPOINT_BOARD_MURPHY_M4
// Opt-in calibration logger for collecting Murphy M4 voltage/runtime data.
// Create BATTERY_LOG_ENABLE_PATH on SD to enable; the RTC-backed wall_epoch column
// makes multi-day battery logs comparable across deep sleep cycles.
constexpr char BATTERY_LOG_ENABLE_PATH[] = "/.crosspoint/battery-log.enable";
constexpr char BATTERY_LOG_PATH[] = "/.crosspoint/battery-log.csv";
constexpr unsigned long BATTERY_LOG_INTERVAL_MS = 5UL * 60UL * 1000UL;

bool murphyBatteryLogEnabled() {
  if (!gpio.deviceIsMurphyM4() || !Storage.ready()) {
    return false;
  }

  HalFile enableFile = Storage.open(BATTERY_LOG_ENABLE_PATH);
  if (!enableFile) {
    return false;
  }
  enableFile.close();
  return true;
}

void appendMurphyBatteryLog(const char* event) {
  static bool wroteHeaderThisBoot = false;
  static bool wroteBootThisBoot = false;

  if (!murphyBatteryLogEnabled()) {
    return;
  }

  HalFile logFile = Storage.open(BATTERY_LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!logFile) {
    return;
  }

  // Write the schema once per boot without querying file size.
  if (!wroteHeaderThisBoot) {
    logFile.print("event,ms,wall_epoch,battery_mv,sense_mv,raw_adc,percent,charging,wifi,cpu_mhz,frontlight_cool,"
                  "frontlight_warm\n");
    wroteHeaderThisBoot = true;
  }

  auto writeSample = [&](const char* sampleEvent) {
    const unsigned long now = millis();
    struct timeval wallTime;
    gettimeofday(&wallTime, nullptr);
    const int rawAdc = analogRead(MURPHY_BATTERY_ADC_PIN);
    const uint32_t senseMv = analogReadMilliVolts(MURPHY_BATTERY_ADC_PIN);
    const uint32_t batteryMv = std::min<uint32_t>(senseMv * 2U, 5000U);
    const uint16_t percent = powerManager.getBatteryPercentage();
    const int charging = gpio.isUsbConnected() ? 1 : 0;
    const int wifiActive = WiFi.getMode() == WIFI_MODE_NULL ? 0 : 1;

    char line[224];
    const int len = snprintf(line, sizeof(line), "%s,%lu,%lld,%lu,%lu,%d,%u,%d,%d,%u,%u,%u\n", sampleEvent, now,
                             static_cast<long long>(wallTime.tv_sec),
                             static_cast<unsigned long>(batteryMv), static_cast<unsigned long>(senseMv), rawAdc,
                             static_cast<unsigned>(percent), charging, wifiActive,
                             static_cast<unsigned>(getCpuFrequencyMhz()),
                             static_cast<unsigned>(SETTINGS.frontlightCoolDuty),
                             static_cast<unsigned>(SETTINGS.frontlightWarmDuty));
    if (len > 0) {
      logFile.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(std::min<int>(len, sizeof(line) - 1)));
    }
  };

  if (!wroteBootThisBoot) {
    writeSample("boot");
    wroteBootThisBoot = true;
    if (strcmp(event, "boot") == 0) {
      logFile.close();
      return;
    }
  }
  writeSample(event);
  logFile.close();
}

void appendMurphyBatteryLogIfDue() {
  static unsigned long lastLogMs = 0;

  if (!gpio.deviceIsMurphyM4() || !Storage.ready()) {
    return;
  }

  const unsigned long now = millis();
  if (lastLogMs != 0 && (now - lastLogMs) < BATTERY_LOG_INTERVAL_MS) {
    return;
  }
  lastLogMs = now;

  appendMurphyBatteryLog("periodic");
}

void flushTtfGlyphCacheForSleep() {
  const ReaderFontCacheStats before = TTF_READER_METRICS.cacheStats();
  if (before.glyphCount == 0) return;

  const uint32_t startMs = millis();
  const bool saved = TTF_READER_METRICS.flushPersistentCache();
  const ReaderFontCacheStats after = TTF_READER_METRICS.cacheStats();

  if (before.persistentDirty) {
    if (saved) {
      LOG_INF("SLP", "TTF glyph cache flushed before sleep glyphs=%u bytes=%u elapsed_ms=%lu",
              static_cast<unsigned>(after.glyphCount), static_cast<unsigned>(after.bytes),
              static_cast<unsigned long>(millis() - startMs));
    } else {
      LOG_ERR("SLP", "TTF glyph cache flush before sleep failed glyphs=%u bytes=%u dirty=1",
              static_cast<unsigned>(before.glyphCount), static_cast<unsigned>(before.bytes));
    }
  } else {
    LOG_DBG("SLP", "TTF glyph cache already clean before sleep glyphs=%u bytes=%u",
            static_cast<unsigned>(before.glyphCount), static_cast<unsigned>(before.bytes));
  }
}
#endif

}  // namespace

namespace {
void applyFrontlightSettings() {
  if (!gpio.deviceIsMurphyM4() || !halFrontlight.isAvailable()) return;
  halFrontlight.set(SETTINGS.frontlightCoolDuty, SETTINGS.frontlightWarmDuty);
}

bool handleFrontlightOverlayInput() {
  if (!gpio.deviceIsMurphyM4() || !halFrontlight.isAvailable()) return false;

  if (gpio.wasFrontlightButtonReleased()) {
    if (activityManager.isCurrentActivity("FrontlightOverlay")) {
      activityManager.popActivity();
    } else {
      activityManager.pushActivity(std::make_unique<FrontlightOverlayActivity>(renderer, mappedInputManager));
    }
    return true;
  }
  return false;
}
}  // namespace

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() < calibratedPressDuration);
    abort = gpio.getPowerButtonHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  activityManager.waitForRenderIdle();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  appendMurphyBatteryLog("sleep");
#endif

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  flushTtfGlyphCacheForSleep();
#endif

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  WifiLifecycle::powerOff("SLP");

  halFrontlight.off();
  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  LOG_INF("DISP", "Display/font setup begin seamless=%d", seamless);
  display.begin(seamless);
  LOG_INF("DISP", "Display begin complete");
  renderer.begin();
  LOG_INF("DISP", "Renderer begin complete");
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  LOG_DBG("MAIN", "Fonts setup");
  LOG_INF("DISP", "Display/font setup complete");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

#ifdef CROSSPOINT_MURPHY_APP1_FLASH_LOG
  const bool flashLogReady = MurphyFlashLog::begin();
#endif

  HalSystem::begin();

#ifdef CROSSPOINT_MURPHY_APP1_FLASH_LOG
  LOG_INF("BOOT", "Murphy app1 flash log ready=%d", flashLogReady);
#endif

  // Read-and-clear so a panic later in setup() does not loop into silent reboot.
  // Bound the target range too; RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  halFrontlight.begin();
  mappedInputManager.beginTouch();
  halEnvSensor.begin();
  powerManager.begin();
  halClock.begin();
  halTiltSensor.begin();

  const auto& board = gpio.getBoardProfile();
  LOG_INF("MAIN",
          "Board profile: %s (%s), soc=%s, display=%ux%u visible=%ux%u, psram=%d budget=%lu, touch=%d/%s, "
          "frontlight=%d channels=%u, rtc=%d, batteryGauge=%d, charger=%d, tilt=%d, env=%d",
          board.label, board.id, socFamilyName(board.socFamily), board.displayWidth, board.displayHeight,
          board.visibleWidth, board.visibleHeight, board.hasPsram, static_cast<unsigned long>(board.psramCacheBudgetBytes),
          board.inputHasTouch, board.touchController, board.hasFrontlight, board.frontlightChannels, board.hasRtc,
          board.hasBatteryGauge, board.hasChargerControl, board.hasTiltSensor, board.hasEnvironmentalSensor);
  HalSystem::logBootDiagnostics(board);

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  const bool storageReady = Storage.begin();
  HalSystem::logStorageDiagnostics(storageReady);
  if (!storageReady) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  halClock.seedSystemTimeFromRTC(SETTINGS.clockUtcOffsetQ);
  applyFrontlightSettings();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  if (resume == BootResume::QuickResume) {
    // One-shot flag: re-arm the splash immediately so a reset during quick
    // resume does not strand the device in a recovery-chord-skipping loop.
    APP_STATE.showBootScreen = true;
    APP_STATE.saveToFile();
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && resume == BootResume::Splash) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      if (loadSleepFrameBuffer()) {
        // Frame restored in memory; leave the retained panel image alone until
        // the routed activity's first paint lands. The loading-icon refresh is
        // a full-screen HALF update on X3 and can cost multiple seconds.
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home; do not fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  if (bootDiagnosticsOnly) {
    if (millis() - lastMemPrint >= 5000) {
      LOG_INF("DIAG", "Heartbeat uptime=%lu storage=%d heap=%u psramFree=%u", millis(), Storage.ready(), ESP.getFreeHeap(),
              ESP.getFreePsram());
      lastMemPrint = millis();
    }
    delay(50);
    return;
  }

  mappedInputManager.setTouchLogicalSize(renderer.getScreenWidth(), renderer.getScreenHeight());
  halTouch.setLogicalOrientation(static_cast<uint8_t>(renderer.getOrientation()));
  mappedInputManager.update();
  if (handleFrontlightOverlayInput()) {
    return;
  }
  if (gpio.wasScreenshotButtonReleased()) {
    if (!activityManager.handleScreenshotRequest()) {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
    }
    return;
  }
  if (gpio.wasSleepButtonReleased()) {
    enterDeepSleep();
    return;
  }
  if (gpio.getBoardProfile().inputHasTouch && mappedInputManager.wasTouchLongPressed()) {
    const auto point = mappedInputManager.lastTouchLongPress();
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const bool centerLongPress = point.x >= screenWidth / 3 && point.x < (screenWidth * 2) / 3 &&
                                 point.y >= screenHeight / 3 && point.y < (screenHeight * 2) / 3;
    if (centerLongPress) {
      LOG_DBG("TOUCH", "Global back long press x=%u y=%u screen=%dx%d", point.x, point.y, screenWidth, screenHeight);
      activityManager.handleGlobalBack();
    }
  }
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || mappedInputManager.hadTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (!gpio.deviceIsMurphyM4() && gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (!gpio.deviceIsMurphyM4() && gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Put the device to sleep when short power button sleep mode is enabled.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  appendMurphyBatteryLogIfDue();
#endif

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
