#include "WifiLifecycle.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace WifiLifecycle {

bool isActive() { return WiFi.getMode() != WIFI_MODE_NULL; }

std::string stationHostname() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return std::string("CrossPoint-Reader-") + mac.c_str();
}

void stopScan(const char* tag) {
  LOG_DBG(tag, "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG(tag, "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());
}

void beginStation(const char* tag) {
  LOG_DBG(tag, "Starting WiFi station mode; free heap=%d bytes", ESP.getFreeHeap());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
}

void beginAccessPoint(const char* tag) {
  LOG_DBG(tag, "Starting WiFi AP mode; free heap=%d bytes", ESP.getFreeHeap());
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
}

void prepareForConnect(const char* tag) {
  beginStation(tag);
  disconnectStation(tag, true);
  delay(100);

  const std::string hostname = stationHostname();
  WiFi.setHostname(hostname.c_str());
  LOG_DBG(tag, "WiFi hostname: %s", hostname.c_str());
}

void disconnectStation(const char* tag, const bool eraseSdkCredentials) {
  LOG_DBG(tag, "Disconnecting WiFi station; eraseSdkCredentials=%d", eraseSdkCredentials ? 1 : 0);
  WiFi.disconnect(eraseSdkCredentials, eraseSdkCredentials);
}

bool disconnectForRestart(const char* tag, const bool apMode) {
  if (!isActive()) {
    LOG_DBG(tag, "WiFi inactive; restart not required");
    return false;
  }

  stopScan(tag);
  if (apMode) {
    LOG_DBG(tag, "Stopping WiFi AP before silent restart...");
    WiFi.softAPdisconnect(true);
  } else {
    disconnectStation(tag, false);
  }
  delay(30);
  LOG_DBG(tag, "WiFi disconnected for restart; free heap=%d bytes", ESP.getFreeHeap());
  return true;
}

void powerOff(const char* tag) {
  if (!isActive()) {
    return;
  }

  stopScan(tag);
  LOG_DBG(tag, "Powering WiFi off");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

}  // namespace WifiLifecycle
