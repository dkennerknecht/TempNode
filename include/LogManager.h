#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

class TimeManager;
class StatsManager;

enum class LogLevel : uint8_t { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

struct LogItem {
  LogLevel level;
  uint32_t createdMs;
  char msg[256];
};

class LogManager {
public:
  void begin(TimeManager& tm, StatsManager& stats, bool sdAvailable);
  void setSdAvailable(bool available);

  void setLevel(LogLevel lvl) { _minLevel = lvl; }
  LogLevel level() const { return _minLevel; }

  void debug(const String& s) { log(LogLevel::DEBUG, s); }
  void info(const String& s)  { log(LogLevel::INFO, s);  }
  void warn(const String& s)  { log(LogLevel::WARN, s);  }
  void error(const String& s) { log(LogLevel::ERROR, s); }

  void log(LogLevel lvl, const String& s);

  // Metrics
  uint32_t drops() const { return _drops; }
  uint32_t queueDepth() const;
  bool sdAvailable() const { return _sdAvailable; }

  // Pause/resume (for OTA)
  void setPaused(bool paused) { _paused = paused; }
  bool paused() const { return _paused; }

private:
  TimeManager* _tm = nullptr;
  StatsManager* _stats = nullptr;

  bool _sdAvailable = false;
  volatile bool _paused = false;

  LogLevel _minLevel = LogLevel::INFO;

  QueueHandle_t _queue = nullptr;
  SemaphoreHandle_t _sdMutex = nullptr;
  TaskHandle_t _task = nullptr;

  uint32_t _drops = 0;
  String _currentFile = "";
  uint16_t _currentDayKey = 0;

  static void taskTrampoline(void* arg);
  void taskMain();

  void rotateIfNeeded();
  void writeLineSerial(const char* line);
  bool writeLineSd(const char* line);
  const char* levelToStr(LogLevel lvl) const;
};
