#include "HistoryManager.h"
#include "LogManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "Config.h"
#include "SensorManager.h"
#include "SharedSdMutex.h"
#include <SD.h>

struct HistoryItem {
  char line[256];
};

void HistoryManager::begin(const HistoryConfig& cfg, LogManager& log, StatsManager& stats, TimeManager& tm, bool sdAvailable) {
  _cfg = &cfg;
  _log = &log;
  _stats = &stats;
  _tm = &tm;
  _sdAvailable = sdAvailable;

  _sdMutex = sharedSdMutex();
  _queue = xQueueCreate(64, sizeof(HistoryItem));
  xTaskCreatePinnedToCore(taskTrampoline, "hist_worker", 4096, this, 1, &_task, 1);
}

void HistoryManager::setSdAvailable(bool available) {
  _sdAvailable = available;
}

void HistoryManager::enqueue(const SensorReading& r) {
  if (_paused) return;
  if (!_cfg->enabled) return;
  if (!_sdAvailable) return;

  HistoryItem it{};
  // JSONL line
  // fields: timestamp, sensorId, tempC, status, timeSource
  // timestamp = epochMs (if NTP) or uptimeMs (if not)
  snprintf(it.line, sizeof(it.line),
           "{\"timestamp\":%llu,\"sensorId\":\"%s\",\"tempC\":%.4f,\"status\":%u,\"timeSource\":%u}",
           (unsigned long long)r.timestampMs, r.id.c_str(), (double)r.tempC, (unsigned)r.status, (unsigned)r.timeSource);

  if (xQueueSend(_queue, &it, 0) != pdTRUE) {
    // drop-oldest
    HistoryItem dummy;
    if (xQueueReceive(_queue, &dummy, 0) == pdTRUE) {
      (void)xQueueSend(_queue, &it, 0);
    }
    _log->warn("history queue overflow: dropped oldest");
  }
}

void HistoryManager::taskTrampoline(void* arg) {
  static_cast<HistoryManager*>(arg)->taskMain();
}

void HistoryManager::taskMain() {
  for (;;) {
    HistoryItem it{};
    if (xQueueReceive(_queue, &it, pdMS_TO_TICKS(200)) != pdTRUE) continue;

    if (!_sdAvailable) continue;

    if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
      _stats->incrementSdWriteFails();
      continue;
    }

    bool ok = false;
    do {
      File f = SD.open(_cfg->path, FILE_APPEND);
      if (!f) break;
      size_t n = f.println(it.line);
      f.flush();
      f.close();
      ok = (n > 0);
    } while (0);

    xSemaphoreGive(_sdMutex);

    if (!ok) {
      _stats->incrementSdWriteFails();
    }
  }
}

bool HistoryManager::readLastLines(const String& sensorId, uint16_t limit, String& out, String& err) {
  if (!_sdAvailable) { err = "SD unavailable"; return false; }
  if (!SD.exists(_cfg->path)) { err = "history file missing"; return false; }

  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    err = "SD busy";
    return false;
  }

  File f = SD.open(_cfg->path, FILE_READ);
  if (!f) {
    xSemaphoreGive(_sdMutex);
    err = "open failed";
    return false;
  }

  // naive tail: read file in chunks from end (safe and bounded)
  const size_t chunk = 1024;
  size_t size = f.size();
  std::vector<String> lines;
  String carry;

  for (size_t pos = size; pos > 0 && lines.size() < (size_t)limit; ) {
    size_t start = (pos >= chunk) ? (pos - chunk) : 0;
    size_t len = pos - start;
    f.seek(start);

    String buf;
    buf.reserve(len + 1);
    for (size_t i = 0; i < len; i++) {
      buf += (char)f.read();
    }

    // split lines
    String s = buf + carry;
    int idx;
    carry = "";
    while ((idx = s.lastIndexOf('\n')) >= 0) {
      String line = s.substring(idx + 1);
      s = s.substring(0, idx);
      line.trim();
      if (!line.length()) continue;
      if (sensorId.length() == 0 || line.indexOf(String("\"sensorId\":\"") + sensorId + "\"") >= 0) {
        lines.push_back(line);
        if (lines.size() >= (size_t)limit) break;
      }
    }
    carry = s;
    pos = start;
  }

  f.close();
  xSemaphoreGive(_sdMutex);

  // reverse to chronological
  out = "[";
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    out += lines[i];
    if (i != 0) out += ",";
  }
  out += "]";
  return true;
}
