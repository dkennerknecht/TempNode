#include "StatsManager.h"
#include "SharedSdMutex.h"
#include <FS.h>
#include <SD.h>

static bool readFileToString(const String& path, String& out) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  out.reserve((size_t)f.size() + 1);
  while (f.available()) {
    out += (char)f.read();
  }
  f.close();
  return true;
}

void StatsManager::begin(bool sdAvailable, const String& path) {
  _mutex = xSemaphoreCreateMutex();
  _sdMutex = sharedSdMutex();

  if (!lock(100)) return;
  _sdAvailable = sdAvailable;
  _path = path;
  _dirty = false;
  _version = 0;
  unlock();
}

bool StatsManager::lock(uint32_t timeoutMs) const {
  if (!_mutex) return false;
  return xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void StatsManager::unlock() const {
  if (_mutex) xSemaphoreGive(_mutex);
}

void StatsManager::setSdAvailable(bool available) {
  if (!lock(100)) return;
  _sdAvailable = available;
  unlock();
}

void StatsManager::markDirty() {
  if (!lock(100)) return;
  markDirtyLocked();
  unlock();
}

void StatsManager::markDirtyLocked() {
  _dirty = true;
  _version++;
}

void StatsManager::incrementCounter(uint32_t PersistedStats::* field) {
  if (!lock(100)) return;
  _stats.*field += 1;
  markDirtyLocked();
  unlock();
}

PersistedStats StatsManager::snapshot() const {
  PersistedStats snap;
  if (!lock(100)) return snap;
  snap = _stats;
  unlock();
  return snap;
}

void StatsManager::setLastResetReason(const String& reason) {
  if (!lock(100)) return;
  _stats.lastResetReason = reason;
  markDirtyLocked();
  unlock();
}

void StatsManager::setLastBootEpochMs(uint64_t epochMs) {
  if (!lock(100)) return;
  _stats.lastBootEpochMs = epochMs;
  markDirtyLocked();
  unlock();
}

void StatsManager::incrementBootCount() {
  incrementCounter(&PersistedStats::bootCount);
}

void StatsManager::incrementSdMountFails() {
  incrementCounter(&PersistedStats::sdMountFails);
}

void StatsManager::incrementSdWriteFails() {
  incrementCounter(&PersistedStats::sdWriteFails);
}

void StatsManager::incrementMqttReconnects() {
  incrementCounter(&PersistedStats::mqttReconnects);
}

void StatsManager::incrementSensorReadErrors() {
  incrementCounter(&PersistedStats::sensorReadErrors);
}

void StatsManager::incrementNtpSyncFails() {
  incrementCounter(&PersistedStats::ntpSyncFails);
}

void StatsManager::toJson(JsonDocument& doc) const {
  doc["bootCount"] = _stats.bootCount;
  doc["sdMountFails"] = _stats.sdMountFails;
  doc["sdWriteFails"] = _stats.sdWriteFails;
  doc["mqttReconnects"] = _stats.mqttReconnects;
  doc["sensorReadErrors"] = _stats.sensorReadErrors;
  doc["ntpSyncFails"] = _stats.ntpSyncFails;
  doc["lastResetReason"] = _stats.lastResetReason;
  doc["lastBootEpochMs"] = (uint64_t)_stats.lastBootEpochMs;
}

void StatsManager::fromJson(JsonDocument& doc) {
  _stats.bootCount = doc["bootCount"] | _stats.bootCount;
  _stats.sdMountFails = doc["sdMountFails"] | _stats.sdMountFails;
  _stats.sdWriteFails = doc["sdWriteFails"] | _stats.sdWriteFails;
  _stats.mqttReconnects = doc["mqttReconnects"] | _stats.mqttReconnects;
  _stats.sensorReadErrors = doc["sensorReadErrors"] | _stats.sensorReadErrors;
  _stats.ntpSyncFails = doc["ntpSyncFails"] | _stats.ntpSyncFails;
  _stats.lastResetReason = String((const char*)(doc["lastResetReason"] | _stats.lastResetReason.c_str()));
  _stats.lastBootEpochMs = (uint64_t)(doc["lastBootEpochMs"] | (uint64_t)_stats.lastBootEpochMs);
}

bool StatsManager::load() {
  if (!_sdMutex) return false;

  String path;
  if (!lock(100)) return false;
  bool sdAvailable = _sdAvailable;
  path = _path;
  unlock();

  if (!sdAvailable) return false;

  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  bool exists = SD.exists(path);
  xSemaphoreGive(_sdMutex);
  if (!exists) return false;

  String s;
  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  bool readOk = readFileToString(path, s);
  xSemaphoreGive(_sdMutex);
  if (!readOk) return false;

  JsonDocument doc;
  auto err = deserializeJson(doc, s);
  if (err) return false;

  if (!lock(100)) return false;
  fromJson(doc);
  _dirty = false;
  _version++;
  unlock();
  return true;
}

bool StatsManager::save() {
  if (!_sdMutex) return false;

  PersistedStats snap;
  String path;
  bool sdAvailable = false;
  bool dirty = false;
  uint32_t version = 0;

  if (!lock(100)) return false;
  sdAvailable = _sdAvailable;
  dirty = _dirty;
  path = _path;
  version = _version;
  snap = _stats;
  unlock();

  if (!sdAvailable) return false;
  if (!dirty) return true;

  if (xSemaphoreTake(_sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;

  String tmp = path + ".tmp";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    xSemaphoreGive(_sdMutex);
    return false;
  }

  JsonDocument doc;
  doc["bootCount"] = snap.bootCount;
  doc["sdMountFails"] = snap.sdMountFails;
  doc["sdWriteFails"] = snap.sdWriteFails;
  doc["mqttReconnects"] = snap.mqttReconnects;
  doc["sensorReadErrors"] = snap.sensorReadErrors;
  doc["ntpSyncFails"] = snap.ntpSyncFails;
  doc["lastResetReason"] = snap.lastResetReason;
  doc["lastBootEpochMs"] = (uint64_t)snap.lastBootEpochMs;

  if (serializeJson(doc, f) == 0) {
    f.close();
    SD.remove(tmp);
    xSemaphoreGive(_sdMutex);
    return false;
  }
  f.flush();
  f.close();

  SD.remove(path);
  if (!SD.rename(tmp, path)) {
    SD.remove(tmp);
    xSemaphoreGive(_sdMutex);
    return false;
  }
  xSemaphoreGive(_sdMutex);

  if (!lock(100)) return false;
  if (_version == version) {
    _dirty = false;
  }
  unlock();
  return true;
}
