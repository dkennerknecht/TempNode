#include "LogManager.h"
#include "Config.h"
#include "TimeManager.h"
#include "StatsManager.h"
#include "SharedSdMutex.h"
#include <SD.h>
#include <WiFi.h>

static constexpr uint32_t kLogRetentionCheckMs = 3600000UL; // hourly
static constexpr uint32_t kSyslogResolveRetryMs = 5000UL;

static uint16_t dayKeyFromLocal(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (uint16_t)((tm.tm_year + 1900) * 366 + (tm.tm_yday + 1));
}

static uint16_t dayKeyFromYmd(int year, int month, int day) {
  struct tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  if (t <= 0) return 0;
  return dayKeyFromLocal(t);
}

static bool parseDailyLogDate(const String& path, int& year, int& month, int& day) {
  String name = path;
  int slash = name.lastIndexOf('/');
  if (slash >= 0) {
    name = name.substring(slash + 1);
  }

  if (name.length() != 16) return false; // log-YYYYMMDD.log
  if (!name.startsWith("log-") || !name.endsWith(".log")) return false;

  char buf[20];
  name.toCharArray(buf, sizeof(buf));
  if (sscanf(buf, "log-%4d%2d%2d.log", &year, &month, &day) != 3) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  return true;
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

void LogManager::configure(const LoggingConfig& cfg) {
  _consoleMinLevel = parseLevel(cfg.consoleLevel, _consoleMinLevel);
  _sdMinLevel = parseLevel(cfg.sdLevel, _sdMinLevel);
  _sdLogEnabled = cfg.sdEnabled;
  _rotateDaily = cfg.rotateDaily;
  _retentionDays = cfg.retentionDays;
  _syslogEnabled = cfg.syslog.enabled;
  _syslogHost = cfg.syslog.host;
  _syslogHost.trim();
  _syslogPort = cfg.syslog.port > 0 ? cfg.syslog.port : 514;
  _syslogMinLevel = parseLevel(cfg.syslog.level, _syslogMinLevel);
  _syslogAppName = cfg.syslog.appName;
  _syslogAppName.trim();
  if (_syslogAppName.isEmpty()) _syslogAppName = "tempnode";
  _syslogFacility = (cfg.syslog.facility <= 23) ? cfg.syslog.facility : 16;
  if (!_syslogEnabled) {
    _syslogUdp.stop();
  }
  resetSyslogResolver();
  _currentFile = "";
  _currentDayKey = 0;
}

LogLevel LogManager::parseLevel(const String& name, LogLevel fallback) {
  String upper = name;
  upper.trim();
  upper.toUpperCase();

  if (upper == "DEBUG") return LogLevel::DEBUG;
  if (upper == "INFO") return LogLevel::INFO;
  if (upper == "WARN") return LogLevel::WARN;
  if (upper == "ERROR") return LogLevel::ERROR;
  return fallback;
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
  enqueue(lvl, s, false);
}

void LogManager::enqueue(LogLevel lvl, const String& s, bool serialAlreadyWritten) {
  if (_paused) return;
  const bool toSerial = !serialAlreadyWritten && ((uint8_t)lvl >= (uint8_t)_consoleMinLevel);
  const bool toSd = _sdAvailable && _sdLogEnabled && ((uint8_t)lvl >= (uint8_t)_sdMinLevel);
  const bool toSyslog = _syslogEnabled && _syslogHost.length() && ((uint8_t)lvl >= (uint8_t)_syslogMinLevel);
  if (!toSerial && !toSd && !toSyslog) return;
  if (!_queue) return;

  LogItem item{};
  item.level = lvl;
  item.createdMs = millis();
  item.serialAlreadyWritten = serialAlreadyWritten;
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

void LogManager::forwardExternalLine(const char* line, bool serialAlreadyWritten) {
  if (!line) return;

  String raw(line);
  raw.replace("\r", "");
  int start = 0;
  while (start < (int)raw.length()) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    String one = raw.substring(start, end);
    one.trim();
    if (one.length()) {
      LogLevel lvl = LogLevel::INFO;
      if (one.indexOf(";ERROR;") >= 0) lvl = LogLevel::ERROR;
      else if (one.indexOf(";WARN;") >= 0) lvl = LogLevel::WARN;
      else if (one.indexOf(";DEBUG;") >= 0) lvl = LogLevel::DEBUG;
      else if (one.startsWith("E (")) lvl = LogLevel::ERROR;
      else if (one.startsWith("W (")) lvl = LogLevel::WARN;
      else if (one.startsWith("D (") || one.startsWith("V (")) lvl = LogLevel::DEBUG;
      enqueue(lvl, one, serialAlreadyWritten);
    }
    start = end + 1;
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

void LogManager::resetSyslogResolver() {
  _syslogIpValid = false;
  _syslogResolvedIp = IPAddress();
  _syslogNextResolveMs = 0;
}

bool LogManager::resolveSyslogHost() {
  if (!_syslogEnabled) return false;
  if (_syslogHost.isEmpty()) return false;
  if (_syslogIpValid) return true;

  const uint32_t now = millis();
  if ((int32_t)(now - _syslogNextResolveMs) < 0) return false;

  IPAddress ip;
  if (ip.fromString(_syslogHost)) {
    _syslogResolvedIp = ip;
    _syslogIpValid = true;
    return true;
  }

  if (WiFi.hostByName(_syslogHost.c_str(), ip)) {
    _syslogResolvedIp = ip;
    _syslogIpValid = true;
    return true;
  }

  _syslogNextResolveMs = now + kSyslogResolveRetryMs;
  return false;
}

bool LogManager::writeLineSyslog(LogLevel lvl, const char* msg, const TimeStamp& ts) {
  if (!_syslogEnabled) return false;
  if (_syslogHost.isEmpty()) return false;
  if (_syslogPort == 0) return false;
  if (!resolveSyslogHost()) return false;

  uint8_t severity = 6; // INFO
  switch (lvl) {
    case LogLevel::DEBUG: severity = 7; break;
    case LogLevel::INFO:  severity = 6; break;
    case LogLevel::WARN:  severity = 4; break;
    case LogLevel::ERROR: severity = 3; break;
  }
  const uint16_t pri = (uint16_t)_syslogFacility * 8U + severity;

  char tsBuf[24];
  if (ts.valid && ts.source == TimeSource::NTP) {
    static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t sec = (time_t)(ts.epochMs / 1000ULL);
    struct tm t;
    localtime_r(&sec, &t);
    const char* mon = (t.tm_mon >= 0 && t.tm_mon < 12) ? kMon[t.tm_mon] : "Jan";
    snprintf(tsBuf, sizeof(tsBuf), "%s %2d %02d:%02d:%02d",
             mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    uint64_t s = ts.epochMs / 1000ULL;
    const uint32_t ss = (uint32_t)(s % 60ULL);
    const uint32_t mm = (uint32_t)((s / 60ULL) % 60ULL);
    const uint32_t hh = (uint32_t)((s / 3600ULL) % 24ULL);
    snprintf(tsBuf, sizeof(tsBuf), "Jan  1 %02u:%02u:%02u", hh, mm, ss);
  }

  const char* host = WiFi.getHostname();
  if (!host || !host[0]) host = "tempnode";

  char payload[420];
  snprintf(payload, sizeof(payload), "<%u>%s %s %s: %s",
           (unsigned)pri, tsBuf, host, _syslogAppName.c_str(), msg);

  if (!_syslogUdp.beginPacket(_syslogResolvedIp, _syslogPort)) return false;
  const size_t payloadLen = strlen(payload);
  const size_t n = _syslogUdp.write((const uint8_t*)payload, payloadLen);
  if (!_syslogUdp.endPacket()) return false;
  return n == payloadLen;
}

void LogManager::rotateIfNeeded() {
  if (!_sdAvailable) return;

  if (!_rotateDaily) {
    _currentFile = "/log.log";
    _currentDayKey = 0;
    return;
  }

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

void LogManager::applyRetentionIfNeeded() {
  if (!_sdAvailable) return;
  if (!_sdLogEnabled) return;
  if (!_rotateDaily) return;
  if (_retentionDays == 0) return;
  if (!_sdMutex) return;

  TimeStamp now = _tm->now();
  if (!now.valid) return;

  const uint64_t retentionMs = (uint64_t)_retentionDays * 24ULL * 60ULL * 60ULL * 1000ULL;
  if (now.epochMs <= retentionMs) return;
  const uint64_t cutoffEpochMs = now.epochMs - retentionMs;
  const uint16_t cutoffDay = dayKeyFromLocal((time_t)(cutoffEpochMs / 1000ULL));

  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    if (_stats) _stats->incrementSdWriteFails();
    return;
  }

  size_t removed = 0;
  File root = SD.open("/");
  if (!root) {
    xSemaphoreGive(_sdMutex);
    if (_stats) _stats->incrementSdWriteFails();
    return;
  }

  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    const bool isDir = f.isDirectory();
    f.close();

    if (!isDir) {
      int year = 0, month = 0, day = 0;
      if (parseDailyLogDate(name, year, month, day)) {
        const uint16_t fileDay = dayKeyFromYmd(year, month, day);
        if (fileDay > 0 && fileDay < cutoffDay) {
          String path = name.startsWith("/") ? name : String("/") + name;
          if (path != _currentFile && SD.remove(path)) {
            removed++;
          }
        }
      }
    }

    f = root.openNextFile();
  }
  root.close();
  xSemaphoreGive(_sdMutex);

  if (removed > 0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "log retention removed files: %u", (unsigned)removed);
    writeLineSerial(msg);
  }
}

void LogManager::taskMain() {
  // optional: add this task to watchdog externally
  uint32_t nextRetentionMs = millis() + 60000UL;
  for (;;) {
    LogItem item{};
    if (xQueueReceive(_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
      uint32_t nowNoItem = millis();
      if ((int32_t)(nowNoItem - nextRetentionMs) >= 0) {
        applyRetentionIfNeeded();
        nextRetentionMs = nowNoItem + kLogRetentionCheckMs;
      }
      continue;
    }

    TimeStamp ts = _tm->now();
    String tss = _tm->formatForLog(ts);

    char line[512];
    snprintf(line, sizeof(line), "%s;%s;%s", tss.c_str(), levelToStr(item.level), item.msg);

    if (!item.serialAlreadyWritten && (uint8_t)item.level >= (uint8_t)_consoleMinLevel) {
      writeLineSerial(line);
    }

    if (_sdAvailable && _sdLogEnabled && (uint8_t)item.level >= (uint8_t)_sdMinLevel) {
      if (!writeLineSd(line)) {
        _stats->incrementSdWriteFails();
      }
    }

    if (_syslogEnabled && _syslogHost.length() && (uint8_t)item.level >= (uint8_t)_syslogMinLevel) {
      (void)writeLineSyslog(item.level, item.msg, ts);
    }

    uint32_t now = millis();
    if ((int32_t)(now - nextRetentionMs) >= 0) {
      applyRetentionIfNeeded();
      nextRetentionMs = now + kLogRetentionCheckMs;
    }
  }
}
