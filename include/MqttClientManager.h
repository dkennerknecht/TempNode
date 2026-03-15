#pragma once
#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <map>
#include <vector>
#include <freertos/semphr.h>

class LogManager;
class StatsManager;
class TimeManager;
class AppNetworkManager;
struct AppConfig;
struct SensorReading;

struct BufferedMsg {
  String topic;
  String payload;
  bool retain = false;
  uint8_t qos = 0;
};

class MqttClientManager {
public:
  void begin(const AppConfig& cfg, AppNetworkManager& net, LogManager& log, StatsManager& stats, TimeManager& tm);
  void loop();

  bool connected() const { return _connected; }
  const String& host() const { return _host; }
  uint16_t port() const { return _port; }
  String authUserLabel() const;
  uint32_t connectAttempts() const { return _connectAttempts; }
  bool lastResolveOk() const { return _diagResolveOk; }
  const String& lastResolvedIp() const { return _diagResolvedIp; }
  bool lastPingOk() const { return _diagPingOk; }
  uint32_t lastPingMs() const { return _diagPingMs; }
  bool lastTcpProbeOpen() const { return _diagTcpOpen; }
  uint16_t lastTcpProbePort() const { return _diagTcpPort; }
  bool lastAltPortChecked() const { return _diagAltChecked; }
  uint16_t lastAltPort() const { return _diagAltPort; }
  bool lastAltPortOpen() const { return _diagAltOpen; }
  uint32_t lastDiagUptimeMs() const { return _diagAtUptimeMs; }
  bool offlineSdEnabled() const { return _sdQueueEnabled; }
  uint32_t offlineSdQueued() const { return _sdQueueLines; }
  uint32_t offlineSdDropped() const { return _sdQueueDropped; }
  void publishSensor(const SensorReading& r);
  void publishSystem();
  void publishHealth();
  void publishStatus(const char* status, bool retain=true);

private:
  const AppConfig* _cfg = nullptr;
  AppNetworkManager* _net = nullptr;
  LogManager* _log = nullptr;
  StatsManager* _stats = nullptr;
  TimeManager* _tm = nullptr;

  AsyncMqttClient _mqtt;

  bool _connected = false;
  uint32_t _nextReconnectMs = 0;
  uint32_t _backoffMs = 1000;

  String _deviceId;
  String _base;

  // AsyncMqttClient keeps pointers to these strings in some setters.
  // Therefore: store them as members (never pass c_str() of temporaries).
  String _clientId;
  String _willTopic;
  String _host;
  uint16_t _port = 1883;
  String _user;
  String _pass;
  bool _tls = false;
  uint32_t _connectAttempts = 0;
  uint32_t _nextHealthPublishMs = 0;

  bool _diagResolveOk = false;
  String _diagResolvedIp;
  bool _diagPingOk = false;
  uint32_t _diagPingMs = 0;
  bool _diagTcpOpen = false;
  uint16_t _diagTcpPort = 0;
  bool _diagAltChecked = false;
  uint16_t _diagAltPort = 0;
  bool _diagAltOpen = false;
  uint32_t _diagAtUptimeMs = 0;

  bool _sdQueueEnabled = false;
  String _sdQueuePath;
  uint32_t _sdQueueMaxLines = 0;
  uint32_t _sdQueueLines = 0;
  uint32_t _sdQueueDropped = 0;
  uint32_t _nextSdQueueFlushMs = 0;
  SemaphoreHandle_t _sdMutex = nullptr;

  // per-sensor ringbuffer
  struct Ring {
    std::vector<BufferedMsg> buf;
    size_t head = 0;
    size_t count = 0;
  };
  std::map<String, Ring> _rings;

  void scheduleReconnect();
  void runConnectivityDiagnostics();
  bool resolveHost(IPAddress& out);
  bool pingIp(const IPAddress& ip, uint32_t timeoutMs, uint32_t& outRttMs);
  bool tcpProbe(const IPAddress& ip, uint16_t port, uint32_t timeoutMs);
  void flushBuffers();
  void recountSdQueueLinesLocked();
  bool appendSdQueuedMessage(const BufferedMsg& m);
  bool parseQueuedLine(const String& line, BufferedMsg& out) const;
  void flushSdQueue();

  String topicTemps(const String& sensorId) const;
  String topicTempsFloat(const String& sensorId) const;
  String topicSystem() const;
  String topicHealth() const;
  String topicStatus() const;

  String readingToJson(const SensorReading& r) const;
  String systemToJson() const;
  String healthToJson() const;

  void ensureRing(const String& sensorId);
  void ringPush(const String& sensorId, const BufferedMsg& m);
};
