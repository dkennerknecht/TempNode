#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <WiFiUdp.h>

class TimeManager;
class StatsManager;
struct LoggingConfig;
struct TimeStamp;

enum class LogLevel : uint8_t { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

struct LogItem {
  LogLevel level;
  uint32_t createdMs;
  bool serialAlreadyWritten;
  char msg[256];
};

class LogManager {
public:
  void begin(TimeManager& tm, StatsManager& stats, bool sdAvailable);
  void setSdAvailable(bool available);
  void configure(const LoggingConfig& cfg);

  static LogLevel parseLevel(const String& name, LogLevel fallback = LogLevel::INFO);

  void setLevel(LogLevel lvl) { _consoleMinLevel = lvl; _sdMinLevel = lvl; }
  void setConsoleLevel(LogLevel lvl) { _consoleMinLevel = lvl; }
  void setSdLevel(LogLevel lvl) { _sdMinLevel = lvl; }
  LogLevel consoleLevel() const { return _consoleMinLevel; }
  LogLevel sdLevel() const { return _sdMinLevel; }

  void debug(const String& s) { log(LogLevel::DEBUG, s); }
  void info(const String& s)  { log(LogLevel::INFO, s);  }
  void warn(const String& s)  { log(LogLevel::WARN, s);  }
  void error(const String& s) { log(LogLevel::ERROR, s); }

  void log(LogLevel lvl, const String& s);
  // Mirrors externally generated lines (e.g. ESP-IDF log output) into SD/syslog.
  // Set serialAlreadyWritten=true when original output was already emitted to serial.
  void forwardExternalLine(const char* line, bool serialAlreadyWritten = true);

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

  LogLevel _consoleMinLevel = LogLevel::INFO;
  LogLevel _sdMinLevel = LogLevel::INFO;
  bool _sdLogEnabled = true;
  bool _rotateDaily = true;
  uint16_t _retentionDays = 0;

  bool _syslogEnabled = false;
  String _syslogHost = "";
  uint16_t _syslogPort = 514;
  LogLevel _syslogMinLevel = LogLevel::INFO;
  String _syslogAppName = "tempnode";
  uint8_t _syslogFacility = 16;
  WiFiUDP _syslogUdp;
  IPAddress _syslogResolvedIp;
  bool _syslogIpValid = false;
  uint32_t _syslogNextResolveMs = 0;

  QueueHandle_t _queue = nullptr;
  SemaphoreHandle_t _sdMutex = nullptr;
  TaskHandle_t _task = nullptr;

  uint32_t _drops = 0;
  String _currentFile = "";
  uint16_t _currentDayKey = 0;

  static void taskTrampoline(void* arg);
  void taskMain();
  void enqueue(LogLevel lvl, const String& s, bool serialAlreadyWritten);

  void rotateIfNeeded();
  void applyRetentionIfNeeded();
  bool resolveSyslogHost();
  bool writeLineSyslog(LogLevel lvl, const char* msg, const TimeStamp& ts);
  void resetSyslogResolver();
  void writeLineSerial(const char* line);
  bool writeLineSd(const char* line);
  const char* levelToStr(LogLevel lvl) const;
};
