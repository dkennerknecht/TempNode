#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <vector>
#include <array>

class LogManager;
class StatsManager;
class TimeManager;
struct SensorConfig;

enum class SensorStatus : uint8_t {
  OK = 0,
  DISCONNECTED = 1,
  CRC_ERROR = 2,
  INVALID_85C = 3,
  INVALID_RANGE = 4,
  TIMEOUT = 5
};

struct SensorReading {
  String id;          // ROM hex
  float tempC = NAN;
  SensorStatus status = SensorStatus::DISCONNECTED;
  uint64_t timestampMs = 0;
  bool timeValid = false;
  uint8_t timeSource = 1; // 0=NTP,1=UPTIME
};

class SensorManager {
public:
  void begin(const SensorConfig& cfg, LogManager& log, StatsManager& stats, TimeManager& tm);
  void loop();

  const std::vector<String>& sensorIds() const { return _ids; }
  bool getLatest(const String& id, SensorReading& out) const;

  // Callback on fresh reading
  using ReadingCallback = std::function<void(const SensorReading&)>;
  void onReading(ReadingCallback cb) { _cb = cb; }

  uint32_t readErrors() const { return _readErrors; }


  // Sampling interval (runtime adjustable)
  uint32_t intervalMs() const { return _intervalMs; }
  const std::vector<SensorReading>& latest() const { return _latest; }
  void setIntervalMs(uint32_t ms);
private:
  const SensorConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  StatsManager* _stats = nullptr;
  TimeManager* _tm = nullptr;

  OneWire* _ow = nullptr;
  DallasTemperature* _dt = nullptr;

  enum class State : uint8_t { DISCOVER, REQUEST, WAIT, READ };
  State _state = State::DISCOVER;

  uint32_t _intervalMs = 10000;
  uint32_t _nextActionMs = 0;
  uint32_t _convStartMs = 0;
  size_t _idx = 0;

  std::vector<String> _ids;
  std::vector<std::array<uint8_t,8>> _addrs;
  std::vector<SensorReading> _latest;

  ReadingCallback _cb;

  uint32_t _readErrors = 0;

  void discover();
  void requestConversionAll();
  bool conversionReady() const;
  void readAll();

  static String addrToHex(const uint8_t* addr);
  static bool isValidTemp(float c, SensorStatus& status);
};
