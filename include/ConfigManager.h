#pragma once
#include <Arduino.h>
#include "Config.h"

class LogManager;

class ConfigManager {
public:
  void begin(LogManager& log);
  const AppConfig& get() const { return _cfg; }

  // Load /config.json from available storage:
  // - LittleFS (/config.json), then
  // - SD (/config.json) as override if present
  bool loadFromSources(bool sdAvailable, bool littleFsAvailable);

  // Update sensors.intervalMs and persist config:
  // - always write LittleFS /config.json
  // - write SD /config.json only if SD is available and file exists
  bool updateSensorIntervalAndPersist(uint32_t requestedMs,
                                      bool sdAvailable,
                                      uint32_t* appliedMs = nullptr,
                                      bool* savedLittleFs = nullptr,
                                      bool* savedSd = nullptr,
                                      String* error = nullptr);

  bool exportConfigJson(String& out, bool redactSecrets) const;

  bool applyFullConfigJsonAndPersist(const String& fullJson,
                                     bool sdAvailable,
                                     bool dryRun,
                                     bool* changed = nullptr,
                                     bool* restartRequired = nullptr,
                                     bool* savedLittleFs = nullptr,
                                     bool* savedSd = nullptr,
                                     String* error = nullptr);

  String deviceId() const;

private:
  LogManager* _log = nullptr;
  AppConfig _cfg;

  void applyDefaults();
  void syncFeatureFlags();
  bool parseJson(const String& json);
  bool validate();
  bool persistCurrentConfig(bool sdAvailable,
                            bool* savedLittleFs,
                            bool* savedSd,
                            String* error);
};
