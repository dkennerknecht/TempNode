#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class LogManager;
class TimeManager;
class AppNetworkManager;
class SensorManager;
class HistoryManager;
class StatsManager;
class MqttClientManager;
struct AppConfig;

class RestServer {
public:
  void begin(const AppConfig& cfg,
             LogManager& log,
             TimeManager& tm,
             AppNetworkManager& net,
             SensorManager& sensors,
             HistoryManager& history,
             StatsManager& stats,
             MqttClientManager& mqtt);

private:
  const AppConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  TimeManager* _tm = nullptr;
  AppNetworkManager* _net = nullptr;
  SensorManager* _sensors = nullptr;
  HistoryManager* _history = nullptr;
  StatsManager* _stats = nullptr;
  MqttClientManager* _mqtt = nullptr;

  AsyncWebServer* _srv = nullptr;

  bool authOk(AsyncWebServerRequest* req) const;
  void setupRoutes();
};
