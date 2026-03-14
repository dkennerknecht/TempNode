#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <array>
#include <mbedtls/sha256.h>

class LogManager;
class TimeManager;
class AppNetworkManager;
class SensorManager;
class HistoryManager;
class StatsManager;
class MqttClientManager;
class ConfigManager;
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
             MqttClientManager& mqtt,
             ConfigManager& cfgManager);

private:
  enum class OtaTarget : uint8_t {
    Firmware,
    Filesystem
  };

  const AppConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  TimeManager* _tm = nullptr;
  AppNetworkManager* _net = nullptr;
  SensorManager* _sensors = nullptr;
  HistoryManager* _history = nullptr;
  StatsManager* _stats = nullptr;
  MqttClientManager* _mqtt = nullptr;
  ConfigManager* _cfgManager = nullptr;

  AsyncWebServer* _srv = nullptr;

  // OTA upload state
  bool _otaRejected = false;
  bool _otaVersionChecked = false;
  size_t _otaExpectedBytes = 0;
  size_t _otaReceivedBytes = 0;
  size_t _otaHeaderBytes = 0;
  bool _otaMd5ExpectedSet = false;
  bool _otaSha256ExpectedSet = false;
  bool _otaSha256Active = false;
  String _otaRejectReason = "";
  std::array<uint8_t, 512> _otaHeaderBuf{};
  std::array<uint8_t, 32> _otaSha256Expected{};
  mbedtls_sha256_context _otaSha256Ctx{};

  bool authOk(AsyncWebServerRequest* req) const;
  void resetOtaState();
  bool otaPrecheck(AsyncWebServerRequest* req, const String& filename, OtaTarget target);
  bool otaFinalize(AsyncWebServerRequest* req, const char* uploadName);
  bool otaCheckVersionChunk(const uint8_t* data, size_t len);
  void setupRoutes();
};
