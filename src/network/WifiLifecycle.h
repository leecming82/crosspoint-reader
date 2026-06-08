#pragma once

#include <string>

namespace WifiLifecycle {

bool isActive();
std::string stationHostname();

void stopScan(const char* tag);
void beginStation(const char* tag);
void beginAccessPoint(const char* tag);
void prepareForConnect(const char* tag);
void disconnectStation(const char* tag, bool eraseSdkCredentials = false);
bool disconnectForRestart(const char* tag, bool apMode);
void powerOff(const char* tag);

}  // namespace WifiLifecycle
