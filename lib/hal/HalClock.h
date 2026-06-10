#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  enum class Backend : uint8_t {
    None,
    Ds3231,
    MurphyRtc32,
  };

  struct DateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
  };

  Backend _backend = Backend::None;
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if a board-supported persistent clock is present on this device.
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Format the hardware RTC's full date/time into "YYYY-MM-DD HH:MM:SS".
  // Returns false when the active clock backend has no validated full date.
  bool formatDateTime(char* buf, size_t bufSize) const;

  // Sync ESP system time from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if system time was successfully updated.
  bool syncSystemTimeFromNTP();

  // Seed ESP system time from the hardware RTC when it has full date/time.
  // Murphy stores local wall time, so rtcUtcOffsetQuarterHoursBiased is used
  // to convert that wall time back to UTC for ESP system time.
  bool seedSystemTimeFromRTC(uint8_t rtcUtcOffsetQuarterHoursBiased = 48);

  // Sync the hardware RTC from an NTP server.
  // X3 stores UTC. Murphy stores wall time using rtcUtcOffsetQuarterHoursBiased so the
  // external RTC remains useful before ESP system time is valid.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the system time was synced and the hardware RTC was updated.
  bool syncFromNTP(uint8_t rtcUtcOffsetQuarterHoursBiased = 48);

 private:
  bool beginDs3231();
  bool beginMurphyRtc32();
  bool getDs3231Time(uint8_t& hour, uint8_t& minute) const;
  bool getMurphyRtc32Time(uint8_t& hour, uint8_t& minute) const;
  bool readMurphyRtc32DateTime(DateTime& dateTime) const;
  bool writeMurphyRtc32DateTime(const DateTime& dateTime);
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
  static DateTime dateTimeFromEpoch(time_t epoch);
  static time_t epochFromDateTime(const DateTime& dateTime);
  static bool dateTimeMatches(const DateTime& lhs, const DateTime& rhs);
};
