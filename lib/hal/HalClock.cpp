#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)
//
// Murphy M4 external RTC register layout observed from stock-set devices:
//   0x10: Seconds  (BCD)
//   0x11: Minutes  (BCD)
//   0x12: Hours    (BCD, 24h)
//   0x13: Week/status-like field
//   0x14: Day      (BCD)
//   0x15: Month    (BCD)
//   0x16: Year     (BCD, 20xx)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }
static constexpr time_t SYSTEM_TIME_VALID_EPOCH_MIN = 1704067200;  // 2024-01-01T00:00:00Z
static constexpr uint8_t MURPHY_RTC_ADDR = 0x32;
static constexpr uint8_t MURPHY_RTC_TIME_REG = 0x10;
static constexpr uint8_t MURPHY_RTC_CTRL1_REG = 0x1F;
static constexpr uint8_t MURPHY_RTC_CTRL2_REG = 0x20;
static constexpr int MURPHY_I2C_SDA = 13;
static constexpr int MURPHY_I2C_SCL = 12;
static constexpr uint32_t MURPHY_I2C_FREQ = 400000;

static bool systemTimeIsValid() { return time(nullptr) >= SYSTEM_TIME_VALID_EPOCH_MIN; }

static int signedOffsetQuarterHours(uint8_t utcOffsetQuarterHoursBiased) {
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 48;
  return static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
}

static int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

static bool writeI2cRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readI2cRegister(uint8_t address, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(address, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  value = Wire.read();
  return true;
}

void HalClock::begin() {
  _backend = Backend::None;
  _available = false;
  _hasCachedTime = false;
  _lastPollMs = 0;

  if (gpio.deviceIsX3()) {
    _available = beginDs3231();
    _backend = _available ? Backend::Ds3231 : Backend::None;
  } else if (gpio.deviceIsMurphyM4()) {
    _available = beginMurphyRtc32();
    _backend = _available ? Backend::MurphyRtc32 : Backend::None;
  }

  if (_available) {
    uint8_t h, m;
    getTime(h, m);
  }
}

bool HalClock::beginDs3231() {
  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    return false;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    return false;
  }
  Wire.read();  // discard — just testing connectivity

  LOG_INF("CLK", "DS3231 RTC found");
  return true;
}

bool HalClock::beginMurphyRtc32() {
  Wire.begin(MURPHY_I2C_SDA, MURPHY_I2C_SCL, MURPHY_I2C_FREQ);
  Wire.setTimeOut(8);

  uint8_t h, m;
  if (!getMurphyRtc32Time(h, m)) {
    LOG_INF("CLK", "Murphy 0x32 RTC not found or invalid");
    return false;
  }

  LOG_INF("CLK", "Murphy 0x32 RTC found");
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  if (_backend == Backend::MurphyRtc32 && systemTimeIsValid()) {
    time_t now = time(nullptr);
    struct tm utcTime;
    gmtime_r(&now, &utcTime);
    hour = static_cast<uint8_t>(utcTime.tm_hour);
    minute = static_cast<uint8_t>(utcTime.tm_min);
    return true;
  }

  if (_backend == Backend::MurphyRtc32) {
    return getMurphyRtc32Time(hour, minute);
  }

  return getDs3231Time(hour, minute);
}

bool HalClock::getDs3231Time(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::getMurphyRtc32Time(uint8_t& hour, uint8_t& minute) const {
  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  DateTime dateTime;
  if (!readMurphyRtc32DateTime(dateTime)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedHour = dateTime.hour;
  _cachedMinute = dateTime.minute;
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::readMurphyRtc32DateTime(DateTime& dateTime) const {
  Wire.beginTransmission(MURPHY_RTC_ADDR);
  Wire.write(MURPHY_RTC_TIME_REG);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(MURPHY_RTC_ADDR, static_cast<uint8_t>(7));
  if (Wire.available() < 7) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  const uint8_t rawSecond = Wire.read();
  const uint8_t rawMinute = Wire.read();
  const uint8_t rawHour = Wire.read();
  Wire.read();  // week/status-like field
  const uint8_t rawDay = Wire.read();
  const uint8_t rawMonth = Wire.read();
  const uint8_t rawYear = Wire.read();

  const uint8_t decodedSecond = bcdToDec(rawSecond & 0x7F);
  const uint8_t decodedMinute = bcdToDec(rawMinute & 0x7F);
  const uint8_t decodedHour = bcdToDec(rawHour & 0x3F);
  const uint8_t decodedDay = bcdToDec(rawDay & 0x3F);
  const uint8_t decodedMonth = bcdToDec(rawMonth & 0x1F);
  const uint8_t decodedYear = bcdToDec(rawYear);
  if (decodedSecond > 59 || decodedMinute > 59 || decodedHour > 23 || decodedDay == 0 || decodedDay > 31 ||
      decodedMonth == 0 || decodedMonth > 12) {
    return false;
  }

  dateTime.year = static_cast<uint16_t>(2000 + decodedYear);
  dateTime.month = decodedMonth;
  dateTime.day = decodedDay;
  dateTime.hour = decodedHour;
  dateTime.minute = decodedMinute;
  dateTime.second = decodedSecond;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m);
  if (_backend != Backend::MurphyRtc32 || systemTimeIsValid()) {
    // Apply UTC offset: convert biased value to signed quarter-hours.
    // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
    if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
    const int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
    totalMinutes += offsetQuarterHours * 15;
  }

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::formatDateTime(char* buf, size_t bufSize) const {
  if (!_available || bufSize < 20) return false;

  if (_backend == Backend::MurphyRtc32) {
    DateTime dateTime;
    if (!readMurphyRtc32DateTime(dateTime)) return false;
    snprintf(buf, bufSize, "%04u-%02u-%02u %02u:%02u:%02u", dateTime.year, dateTime.month, dateTime.day,
             dateTime.hour, dateTime.minute, dateTime.second);
    return true;
  }

  return false;
}

HalClock::DateTime HalClock::dateTimeFromEpoch(const time_t epoch) {
  struct tm timeinfo;
  gmtime_r(&epoch, &timeinfo);
  return DateTime{static_cast<uint16_t>(timeinfo.tm_year + 1900),
                  static_cast<uint8_t>(timeinfo.tm_mon + 1),
                  static_cast<uint8_t>(timeinfo.tm_mday),
                  static_cast<uint8_t>(timeinfo.tm_hour),
                  static_cast<uint8_t>(timeinfo.tm_min),
                  static_cast<uint8_t>(timeinfo.tm_sec)};
}

time_t HalClock::epochFromDateTime(const DateTime& dateTime) {
  const int64_t days = daysFromCivil(dateTime.year, dateTime.month, dateTime.day);
  const int64_t seconds = days * 86400 + static_cast<int64_t>(dateTime.hour) * 3600 +
                          static_cast<int64_t>(dateTime.minute) * 60 + dateTime.second;
  return static_cast<time_t>(seconds);
}

bool HalClock::dateTimeMatches(const DateTime& lhs, const DateTime& rhs) {
  return lhs.year == rhs.year && lhs.month == rhs.month && lhs.day == rhs.day && lhs.hour == rhs.hour &&
         lhs.minute == rhs.minute && lhs.second == rhs.second;
}

bool HalClock::writeMurphyRtc32DateTime(const DateTime& dateTime) {
  if (dateTime.year < 2000 || dateTime.year > 2099 || dateTime.month == 0 || dateTime.month > 12 ||
      dateTime.day == 0 || dateTime.day > 31 || dateTime.hour > 23 || dateTime.minute > 59 || dateTime.second > 59) {
    LOG_ERR("CLK", "Refusing invalid Murphy RTC write %04u-%02u-%02u %02u:%02u:%02u", dateTime.year,
            dateTime.month, dateTime.day, dateTime.hour, dateTime.minute, dateTime.second);
    return false;
  }

  uint8_t ctrl1Before = 0;
  uint8_t ctrl2Before = 0;
  readI2cRegister(MURPHY_RTC_ADDR, MURPHY_RTC_CTRL1_REG, ctrl1Before);
  readI2cRegister(MURPHY_RTC_ADDR, MURPHY_RTC_CTRL2_REG, ctrl2Before);

  // SD3078-family write unlock, shifted by +0x10 for Murphy's observed register bank.
  if (!writeI2cRegister(MURPHY_RTC_ADDR, MURPHY_RTC_CTRL2_REG, ctrl2Before | 0x80) ||
      !writeI2cRegister(MURPHY_RTC_ADDR, MURPHY_RTC_CTRL1_REG, ctrl1Before | 0x04) ||
      !writeI2cRegister(MURPHY_RTC_ADDR, MURPHY_RTC_CTRL1_REG, ctrl1Before | 0x84)) {
    LOG_ERR("CLK", "Murphy RTC write unlock failed");
    return false;
  }

  Wire.beginTransmission(MURPHY_RTC_ADDR);
  Wire.write(MURPHY_RTC_TIME_REG);
  Wire.write(decToBcd(dateTime.second));
  Wire.write(decToBcd(dateTime.minute));
  Wire.write(decToBcd(dateTime.hour));
  Wire.write(0x10);  // Preserve observed stock/status weekday field shape.
  Wire.write(decToBcd(dateTime.day));
  Wire.write(decToBcd(dateTime.month));
  Wire.write(decToBcd(static_cast<uint8_t>(dateTime.year - 2000)));
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Murphy RTC time write failed");
    return false;
  }

  DateTime after;
  if (!readMurphyRtc32DateTime(after)) {
    LOG_ERR("CLK", "Murphy RTC readback failed after write");
    return false;
  }

  const DateTime oneSecondLater =
      (dateTime.second < 59) ? DateTime{dateTime.year, dateTime.month, dateTime.day, dateTime.hour, dateTime.minute,
                                        static_cast<uint8_t>(dateTime.second + 1)}
                             : dateTime;
  const bool verified = dateTimeMatches(after, dateTime) || dateTimeMatches(after, oneSecondLater);
  if (!verified) {
    LOG_ERR("CLK", "Murphy RTC write verification failed, readback %04u-%02u-%02u %02u:%02u:%02u", after.year,
            after.month, after.day, after.hour, after.minute, after.second);
    return false;
  }

  _lastPollMs = 0;
  _cachedHour = after.hour;
  _cachedMinute = after.minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncSystemTimeFromNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting system time NTP sync...");
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      if (now < SYSTEM_TIME_VALID_EPOCH_MIN) {
        LOG_ERR("CLK", "NTP sync completed but system time is invalid");
        return false;
      }
      LOG_INF("CLK", "System time synced from NTP");
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

bool HalClock::seedSystemTimeFromRTC(const uint8_t rtcUtcOffsetQuarterHoursBiased) {
  if (!_available || _backend != Backend::MurphyRtc32) return false;

  DateTime localDateTime;
  if (!readMurphyRtc32DateTime(localDateTime)) {
    return false;
  }

  const int offsetQuarterHours = signedOffsetQuarterHours(rtcUtcOffsetQuarterHoursBiased);
  const time_t utcEpoch = epochFromDateTime(localDateTime) - static_cast<time_t>(offsetQuarterHours) * 15 * 60;
  if (utcEpoch < SYSTEM_TIME_VALID_EPOCH_MIN) {
    LOG_ERR("CLK", "Murphy RTC time is invalid for system seed");
    return false;
  }

  timeval tv{};
  tv.tv_sec = utcEpoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) {
    LOG_ERR("CLK", "Failed to seed system time from Murphy RTC");
    return false;
  }

  LOG_INF("CLK", "System time seeded from Murphy RTC %04u-%02u-%02u %02u:%02u:%02u offsetQ=%d", localDateTime.year,
          localDateTime.month, localDateTime.day, localDateTime.hour, localDateTime.minute, localDateTime.second,
          offsetQuarterHours);
  return true;
}

bool HalClock::syncFromNTP(uint8_t rtcUtcOffsetQuarterHoursBiased) {
  if (!_available) return false;

  if (!syncSystemTimeFromNTP()) {
    return false;
  }

  if (_backend == Backend::MurphyRtc32) {
    const int offsetQuarterHours = signedOffsetQuarterHours(rtcUtcOffsetQuarterHoursBiased);
    const time_t localEpoch = time(nullptr) + static_cast<time_t>(offsetQuarterHours) * 15 * 60;
    const DateTime localDateTime = dateTimeFromEpoch(localEpoch);
    if (writeMurphyRtc32DateTime(localDateTime)) {
      LOG_INF("CLK", "Murphy RTC set to %04u-%02u-%02u %02u:%02u:%02u offsetQ=%d", localDateTime.year,
              localDateTime.month, localDateTime.day, localDateTime.hour, localDateTime.minute, localDateTime.second,
              offsetQuarterHours);
      return true;
    }
    return false;
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  if (writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
    LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
  }
  return false;
}
