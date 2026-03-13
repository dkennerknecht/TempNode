#pragma once
#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <map>
#include <vector>

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
  void publishSensor(const SensorReading& r);
  void publishSystem();
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
  String _user;
  String _pass;
  bool _tls = false;

  // per-sensor ringbuffer
  struct Ring {
    std::vector<BufferedMsg> buf;
    size_t head = 0;
    size_t count = 0;
  };
  std::map<String, Ring> _rings;

  void scheduleReconnect();
  void flushBuffers();

  String topicTemps(const String& sensorId) const;
  String topicSystem() const;
  String topicStatus() const;

  String readingToJson(const SensorReading& r) const;
  String systemToJson() const;

  void ensureRing(const String& sensorId);
  void ringPush(const String& sensorId, const BufferedMsg& m);
};
