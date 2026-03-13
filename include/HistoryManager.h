#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>

class LogManager;
class StatsManager;
class TimeManager;
struct HistoryConfig;
struct SensorReading;

class HistoryManager {
public:
  void begin(const HistoryConfig& cfg, LogManager& log, StatsManager& stats, TimeManager& tm, bool sdAvailable);
  void setSdAvailable(bool available);

  void enqueue(const SensorReading& r);

  // REST helper
  bool readLastLines(const String& sensorId, uint16_t limit, String& out, String& err);

  void setPaused(bool paused) { _paused = paused; }

private:
  const HistoryConfig* _cfg = nullptr;
  LogManager* _log = nullptr;
  StatsManager* _stats = nullptr;
  TimeManager* _tm = nullptr;

  bool _sdAvailable = false;
  volatile bool _paused = false;

  QueueHandle_t _queue = nullptr;
  SemaphoreHandle_t _sdMutex = nullptr;
  TaskHandle_t _task = nullptr;

  static void taskTrampoline(void* arg);
  void taskMain();
};
