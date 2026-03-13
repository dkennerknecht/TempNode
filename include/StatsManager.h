#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct PersistedStats {
  uint32_t bootCount = 0;
  uint32_t sdMountFails = 0;
  uint32_t sdWriteFails = 0;
  uint32_t mqttReconnects = 0;
  uint32_t sensorReadErrors = 0;
  uint32_t ntpSyncFails = 0;

  String lastResetReason = "";
  uint64_t lastBootEpochMs = 0; // if time valid at boot
};

class StatsManager {
public:
  void begin(bool sdAvailable, const String& path = "/stats.json");
  void setSdAvailable(bool available);

  PersistedStats snapshot() const;

  void setLastResetReason(const String& reason);
  void setLastBootEpochMs(uint64_t epochMs);

  void incrementBootCount();
  void incrementSdMountFails();
  void incrementSdWriteFails();
  void incrementMqttReconnects();
  void incrementSensorReadErrors();
  void incrementNtpSyncFails();

  void markDirty();
  bool load();
  bool save(); // safe write temp+rename, no blocking beyond short timeouts

private:
  SemaphoreHandle_t _mutex = nullptr;
  SemaphoreHandle_t _sdMutex = nullptr;
  bool _sdAvailable = false;
  String _path = "/stats.json";
  bool _dirty = false;
  uint32_t _version = 0;
  PersistedStats _stats;

  bool lock(uint32_t timeoutMs = 100) const;
  void unlock() const;

  void markDirtyLocked();
  void incrementCounter(uint32_t PersistedStats::* field);

  void toJson(JsonDocument& doc) const;
  void fromJson(JsonDocument& doc);
};
