#include "LogManager.h"
#include "TimeManager.h"
#include "StatsManager.h"
#include "SharedSdMutex.h"
#include <SD.h>

static uint16_t dayKeyFromLocal(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (uint16_t)((tm.tm_year + 1900) * 366 + (tm.tm_yday + 1));
}

void LogManager::begin(TimeManager& tm, StatsManager& stats, bool sdAvailable) {
  _tm = &tm;
  _stats = &stats;
  _sdAvailable = sdAvailable;

  _sdMutex = sharedSdMutex();
  _queue = xQueueCreate(64, sizeof(LogItem)); // bounded

  xTaskCreatePinnedToCore(taskTrampoline, "log_worker", 4096, this, 1, &_task, 1);
}

void LogManager::setSdAvailable(bool available) {
  _sdAvailable = available;
}

uint32_t LogManager::queueDepth() const {
  if (!_queue) return 0;
  return (uint32_t)uxQueueMessagesWaiting(_queue);
}

const char* LogManager::levelToStr(LogLevel lvl) const {
  switch (lvl) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    default:              return "ERROR";
  }
}

void LogManager::log(LogLevel lvl, const String& s) {
  if (_paused) return;
  if ((uint8_t)lvl < (uint8_t)_minLevel) return;
  if (!_queue) return;

  LogItem item{};
  item.level = lvl;
  item.createdMs = millis();
  s.substring(0, sizeof(item.msg) - 1).toCharArray(item.msg, sizeof(item.msg));

  // Drop-oldest on overflow
  if (xQueueSend(_queue, &item, 0) != pdTRUE) {
    LogItem dummy;
    if (xQueueReceive(_queue, &dummy, 0) == pdTRUE) {
      (void)xQueueSend(_queue, &item, 0);
    }
    _drops++;
    // best-effort warning directly to serial (non-blocking)
    Serial.println("[LOG] queue overflow: dropped oldest");
  }
}

void LogManager::taskTrampoline(void* arg) {
  static_cast<LogManager*>(arg)->taskMain();
}

void LogManager::writeLineSerial(const char* line) {
  Serial.println(line);
}

bool LogManager::writeLineSd(const char* line) {
  if (!_sdAvailable) return false;
  if (!_sdMutex) return false;
  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;

  bool ok = false;
  do {
    rotateIfNeeded();
    if (_currentFile.isEmpty()) break;

    File f = SD.open(_currentFile, FILE_APPEND);
    if (!f) break;

    size_t n = f.println(line);
    f.flush();
    f.close();
    ok = (n > 0);
  } while (0);

  xSemaphoreGive(_sdMutex);
  return ok;
}

void LogManager::rotateIfNeeded() {
  if (!_sdAvailable) return;

  TimeStamp ts = _tm->now();
  if (!ts.valid) {
    if (_currentFile.isEmpty()) _currentFile = "/log-uptime.log";
    return;
  }
  time_t sec = (time_t)(ts.epochMs / 1000ULL);
  uint16_t dayKey = dayKeyFromLocal(sec);
  if (dayKey == _currentDayKey && !_currentFile.isEmpty()) return;

  struct tm t;
  localtime_r(&sec, &t);
  char name[32];
  snprintf(name, sizeof(name), "/log-%04d%02d%02d.log", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  _currentFile = String(name);
  _currentDayKey = dayKey;
}

void LogManager::taskMain() {
  // optional: add this task to watchdog externally
  for (;;) {
    LogItem item{};
    if (xQueueReceive(_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
      continue;
    }

    TimeStamp ts = _tm->now();
    String tss = _tm->formatForLog(ts);

    char line[512];
    snprintf(line, sizeof(line), "%s;%s;%s", tss.c_str(), levelToStr(item.level), item.msg);

    writeLineSerial(line);

    if (_sdAvailable) {
      if (!writeLineSd(line)) {
        _stats->incrementSdWriteFails();
      }
    }
  }
}
