#pragma once
#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <SPI.h>
#include <driver/spi_master.h>

class LogManager;
class TimeManager;
class AppConfig;

class AppNetworkManager {
public:
  void begin(const AppConfig& cfg, LogManager& log, TimeManager& timeMgr);
  void loop();

  bool isUp() const { return _hasIp; }
  bool linkUp() const { return _linkUp; }
  IPAddress ip() const { return ETH.localIP(); }
  String macStr() const;

  void requestReconnect();

private:
  uint32_t _lastTryMs = 0;

  bool _starting = false;
  bool _started = false;

  const AppConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  TimeManager* _tm = nullptr;

  SPIClass _ethSpi;
  volatile bool _linkUp = false;
  volatile bool _hasIp = false;

  bool _reconnectRequested = false;
  uint32_t _nextRetryMs = 0;
  uint8_t _attempt = 0;

  static void onEvent(arduino_event_id_t event, arduino_event_info_t info);
  static AppNetworkManager* _self;
  void handleEvent(arduino_event_id_t event);

  bool startEth();
  void stopEth();
};
