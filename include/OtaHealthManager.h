#pragma once
#include <Arduino.h>

class LogManager;
class AppNetworkManager;
struct OtaConfig;

class OtaHealthManager {
public:
  void begin(const OtaConfig& cfg, LogManager& log, AppNetworkManager& net);
  void loop();

private:
  const OtaConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  AppNetworkManager* _net = nullptr;

  bool _pendingVerify = false;
  uint32_t _bootMs = 0;
};
