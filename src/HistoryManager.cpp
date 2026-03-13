#include "HistoryManager.h"
#include "LogManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "Config.h"
#include "SensorManager.h"
#include "SharedSdMutex.h"
#include "TempNodeCore.h"
#include <SD.h>

struct HistoryItem {
  char line[256];
};

static constexpr size_t kHistoryBatchMax = 32;
static constexpr uint32_t kRetentionCheckMs = 3600000UL; // hourly

static bool isLikelyEpochMs(uint64_t tsMs) {
  // 2000-01-01 00:00:00 UTC in ms
  return tsMs >= 946684800000ULL;
}

static bool writeBatchLocked(const HistoryConfig& cfg, const std::vector<HistoryItem>& pending) {
  if (pending.empty()) return true;

  File f = SD.open(cfg.path, FILE_APPEND);
  if (!f) return false;

  bool ok = true;
  for (const auto& it : pending) {
    if (f.println(it.line) == 0) {
      ok = false;
      break;
    }
  }
  if (ok) f.flush();
  f.close();
  return ok;
}

static bool applyRetentionLocked(const HistoryConfig& cfg, TimeManager& tm, LogManager& log) {
  if (cfg.retentionDays == 0) return true;
  if (!SD.exists(cfg.path)) return true;

  TimeStamp now = tm.now();
  if (!now.valid) return true;

  const uint64_t retentionMs = (uint64_t)cfg.retentionDays * 24ULL * 60ULL * 60ULL * 1000ULL;
  if (now.epochMs <= retentionMs) return true;
  const uint64_t cutoff = now.epochMs - retentionMs;

  File in = SD.open(cfg.path, FILE_READ);
  if (!in) return false;

  String tmpPath = cfg.path + ".tmp";
  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }

  size_t kept = 0;
  size_t dropped = 0;

  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    bool keep = true;
    uint64_t tsMs = 0;
    if (tempnode::parseHistoryTimestampMs(line.c_str(), tsMs)) {
      // Only prune entries with valid epoch timestamps.
      if (isLikelyEpochMs(tsMs) && tsMs < cutoff) {
        keep = false;
      }
    }

    if (keep) {
      if (out.println(line) == 0) {
        out.close();
        in.close();
        SD.remove(tmpPath);
        return false;
      }
      kept++;
    } else {
      dropped++;
    }
  }

  out.flush();
  out.close();
  in.close();

  if (dropped == 0) {
    SD.remove(tmpPath);
    return true;
  }

  SD.remove(cfg.path);
  if (!SD.rename(tmpPath, cfg.path)) {
    SD.remove(tmpPath);
    return false;
  }

  log.info(String("history retention pruned lines: ") + (unsigned)dropped + ", kept: " + (unsigned)kept);
  return true;
}

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
  std::vector<HistoryItem> pending;
  pending.reserve(kHistoryBatchMax);

  uint32_t flushIntervalMs = _cfg->flushIntervalMs;
  uint32_t nextFlushMs = millis() + (flushIntervalMs == 0 ? 1 : flushIntervalMs);
  uint32_t nextRetentionMs = millis() + 60000UL; // first check after one minute

  for (;;) {
    HistoryItem it{};
    TickType_t waitTicks = pdMS_TO_TICKS(200);
    uint32_t now = millis();
    if (!pending.empty() && flushIntervalMs > 0) {
      int32_t remain = (int32_t)(nextFlushMs - now);
      if (remain < 0) remain = 0;
      if ((uint32_t)remain < 200) waitTicks = pdMS_TO_TICKS((uint32_t)remain);
    }

    if (xQueueReceive(_queue, &it, waitTicks) == pdTRUE) {
      if (_sdAvailable && _cfg->enabled && !_paused) {
        pending.push_back(it);
      }
    }

    now = millis();
    flushIntervalMs = _cfg->flushIntervalMs;
    bool flushDue = !pending.empty() &&
                    (flushIntervalMs == 0 ||
                     (int32_t)(now - nextFlushMs) >= 0 ||
                     pending.size() >= kHistoryBatchMax);

    if (flushDue && _sdAvailable) {
      if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        _stats->incrementSdWriteFails();
      } else {
        if (!writeBatchLocked(*_cfg, pending)) {
          _stats->incrementSdWriteFails();
        }
        xSemaphoreGive(_sdMutex);
      }
      pending.clear();
      nextFlushMs = now + (flushIntervalMs == 0 ? 1 : flushIntervalMs);
    }

    if (_cfg->retentionDays > 0 && _sdAvailable && (int32_t)(now - nextRetentionMs) >= 0) {
      if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        _stats->incrementSdWriteFails();
      } else {
        if (!applyRetentionLocked(*_cfg, *_tm, *_log)) {
          _stats->incrementSdWriteFails();
        }
        xSemaphoreGive(_sdMutex);
      }
      nextRetentionMs = now + kRetentionCheckMs;
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
