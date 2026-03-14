#include "ConfigManager.h"
#include "LogManager.h"
#include "SharedSdMutex.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>

static bool readFileToString(fs::FS& fs, const char* path, String& out) {
  File f = fs.open(path, FILE_READ);
  if (!f) return false;
  out = "";
  out.reserve((size_t)f.size() + 1);
  while (f.available()) out += (char)f.read();
  f.close();
  return true;
}

static bool writeStringAtomic(fs::FS& fs, const char* path, const String& content) {
  const String tmp = String(path) + ".tmp";
  fs.remove(tmp);

  File f = fs.open(tmp, FILE_WRITE);
  if (!f) return false;

  const size_t written = f.print(content);
  f.flush();
  f.close();
  if (written != content.length()) {
    fs.remove(tmp);
    return false;
  }

  fs.remove(path);
  if (!fs.rename(tmp, path)) {
    fs.remove(tmp);
    return false;
  }
  return true;
}

static void configToJson(const AppConfig& cfg, JsonDocument& doc) {
  doc["network"]["hostname"] = cfg.network.hostname;
  doc["network"]["dhcp"] = cfg.network.dhcp;
  doc["network"]["ip"] = cfg.network.ip;
  doc["network"]["gw"] = cfg.network.gw;
  doc["network"]["mask"] = cfg.network.mask;
  doc["network"]["dns"] = cfg.network.dns;

  doc["sensors"]["intervalMs"] = cfg.sensors.intervalMs;
  doc["sensors"]["resolutionBits"] = cfg.sensors.resolutionBits;
  doc["sensors"]["conversionTimeoutMs"] = cfg.sensors.conversionTimeoutMs;

  doc["rest"]["enabled"] = cfg.rest.enabled;
  doc["rest"]["port"] = cfg.rest.port;

  doc["mqtt"]["enabled"] = cfg.mqtt.enabled;
  doc["mqtt"]["host"] = cfg.mqtt.host;
  doc["mqtt"]["port"] = cfg.mqtt.port;
  doc["mqtt"]["tls"] = cfg.mqtt.tls;
  doc["mqtt"]["user"] = cfg.mqtt.user;
  doc["mqtt"]["pass"] = cfg.mqtt.pass;
  doc["mqtt"]["clientId"] = cfg.mqtt.clientId;
  doc["mqtt"]["deviceId"] = cfg.mqtt.deviceId;
  doc["mqtt"]["baseTopic"] = cfg.mqtt.baseTopic;
  doc["mqtt"]["offlineBufferPerSensor"] = cfg.mqtt.offlineBufferPerSensor;
  doc["mqtt"]["publishHealth"] = cfg.mqtt.publishHealth;
  doc["mqtt"]["healthIntervalMs"] = cfg.mqtt.healthIntervalMs;
  doc["mqtt"]["reconnectMinMs"] = cfg.mqtt.reconnectMinMs;
  doc["mqtt"]["reconnectMaxMs"] = cfg.mqtt.reconnectMaxMs;

  doc["history"]["enabled"] = cfg.history.enabled;
  doc["history"]["path"] = cfg.history.path;
  doc["history"]["flushIntervalMs"] = cfg.history.flushIntervalMs;
  doc["history"]["retentionDays"] = cfg.history.retentionDays;

  doc["logging"]["consoleLevel"] = cfg.logging.consoleLevel;
  doc["logging"]["sdLevel"] = cfg.logging.sdLevel;
  doc["logging"]["sdEnabled"] = cfg.logging.sdEnabled;
  doc["logging"]["rotateDaily"] = cfg.logging.rotateDaily;
  doc["logging"]["retentionDays"] = cfg.logging.retentionDays;

  doc["metrics"]["enabled"] = cfg.metrics.enabled;

  doc["ota"]["enabled"] = cfg.ota.enabled;
  doc["ota"]["allowInsecureHttp"] = cfg.ota.allowInsecureHttp;
  doc["ota"]["allowDowngrade"] = cfg.ota.allowDowngrade;
  doc["ota"]["requireHashHeader"] = cfg.ota.requireHashHeader;
  doc["ota"]["healthConfirmMs"] = cfg.ota.healthConfirmMs;
  doc["ota"]["requireNetworkForConfirm"] = cfg.ota.requireNetworkForConfirm;

  doc["watchdog"]["enabled"] = cfg.watchdog.enabled;
  doc["watchdog"]["timeoutMs"] = cfg.watchdog.timeoutMs;
  doc["watchdog"]["panicReset"] = cfg.watchdog.panicReset;

  doc["security"]["enabled"] = cfg.security.enabled;
  doc["security"]["restAuthMode"] = cfg.security.restAuthMode;
  doc["security"]["allowAnonymousGet"] = cfg.security.allowAnonymousGet;
  doc["security"]["restUser"] = cfg.security.restUser;
  doc["security"]["restPass"] = cfg.security.restPass;
  doc["security"]["restToken"] = cfg.security.restToken;
  doc["security"]["mqttUser"] = cfg.security.mqttUser;
  doc["security"]["mqttPass"] = cfg.security.mqttPass;
}

static bool isValidLogLevelName(const String& level) {
  return level == "DEBUG" || level == "INFO" || level == "WARN" || level == "ERROR";
}

static bool normalizeLogLevelName(String& level) {
  level.trim();
  level.toUpperCase();
  if (!isValidLogLevelName(level)) return false;
  return true;
}

void ConfigManager::begin(LogManager& log) {
  _log = &log;
  applyDefaults();
}

void ConfigManager::applyDefaults() {
  _cfg = AppConfig{};
  _cfg.security.restAuthMode = "anonymous";
  _cfg.security.allowAnonymousGet = false;
  _cfg.mqtt.publishHealth = true;
  _cfg.mqtt.healthIntervalMs = 15000;
  syncFeatureFlags();
}

void ConfigManager::syncFeatureFlags() {
  _cfg.restEnabled = _cfg.rest.enabled;
  _cfg.mqttEnabled = _cfg.mqtt.enabled;
  _cfg.securityEnabled = _cfg.security.enabled;
  _cfg.otaEnabled = _cfg.ota.enabled;
  _cfg.metricsEnabled = _cfg.metrics.enabled;
  _cfg.historyEnabled = _cfg.history.enabled;
}

String ConfigManager::deviceId() const {
  if (_cfg.mqtt.deviceId.length()) return _cfg.mqtt.deviceId;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool ConfigManager::parseJson(const String& json) {
  JsonDocument doc;
  auto err = deserializeJson(doc, json);
  if (err) {
    _log->error(String("config.json parse error: ") + err.c_str());
    return false;
  }

  // Feature flags
  _cfg.rest.enabled    = doc["rest"]["enabled"]    | _cfg.rest.enabled;
  _cfg.mqtt.enabled    = doc["mqtt"]["enabled"]    | _cfg.mqtt.enabled;
  _cfg.security.enabled= doc["security"]["enabled"]| _cfg.security.enabled;
  _cfg.ota.enabled     = doc["ota"]["enabled"]     | _cfg.ota.enabled;
  _cfg.ota.allowInsecureHttp = doc["ota"]["allowInsecureHttp"] | _cfg.ota.allowInsecureHttp;
  _cfg.ota.allowDowngrade = doc["ota"]["allowDowngrade"] | _cfg.ota.allowDowngrade;
  _cfg.ota.requireHashHeader = doc["ota"]["requireHashHeader"] | _cfg.ota.requireHashHeader;
  _cfg.ota.healthConfirmMs = doc["ota"]["healthConfirmMs"] | _cfg.ota.healthConfirmMs;
  _cfg.ota.requireNetworkForConfirm = doc["ota"]["requireNetworkForConfirm"] | _cfg.ota.requireNetworkForConfirm;
  _cfg.metrics.enabled = doc["metrics"]["enabled"] | _cfg.metrics.enabled;
  _cfg.history.enabled = doc["history"]["enabled"] | _cfg.history.enabled;

  // Network
  _cfg.network.hostname = String((const char*)(doc["network"]["hostname"] | _cfg.network.hostname.c_str()));
  _cfg.network.dhcp = doc["network"]["dhcp"] | _cfg.network.dhcp;
  _cfg.network.ip   = String((const char*)(doc["network"]["ip"]   | _cfg.network.ip.c_str()));
  _cfg.network.gw   = String((const char*)(doc["network"]["gw"]   | _cfg.network.gw.c_str()));
  _cfg.network.mask = String((const char*)(doc["network"]["mask"] | _cfg.network.mask.c_str()));
  _cfg.network.dns  = String((const char*)(doc["network"]["dns"]  | _cfg.network.dns.c_str()));

  // Sensors
  _cfg.sensors.intervalMs = doc["sensors"]["intervalMs"] | _cfg.sensors.intervalMs;
  _cfg.sensors.resolutionBits = doc["sensors"]["resolutionBits"] | _cfg.sensors.resolutionBits;
  _cfg.sensors.conversionTimeoutMs = doc["sensors"]["conversionTimeoutMs"] | _cfg.sensors.conversionTimeoutMs;

  // MQTT
  _cfg.mqtt.host = String((const char*)(doc["mqtt"]["host"] | _cfg.mqtt.host.c_str()));
  _cfg.mqtt.port = doc["mqtt"]["port"] | _cfg.mqtt.port;
  _cfg.mqtt.clientId = String((const char*)(doc["mqtt"]["clientId"] | _cfg.mqtt.clientId.c_str()));
  _cfg.mqtt.deviceId = String((const char*)(doc["mqtt"]["deviceId"] | _cfg.mqtt.deviceId.c_str()));
  _cfg.mqtt.baseTopic = String((const char*)(doc["mqtt"]["baseTopic"] | _cfg.mqtt.baseTopic.c_str()));
  _cfg.mqtt.offlineBufferPerSensor = doc["mqtt"]["offlineBufferPerSensor"] | _cfg.mqtt.offlineBufferPerSensor;
  _cfg.mqtt.reconnectMinMs = doc["mqtt"]["reconnectMinMs"] | _cfg.mqtt.reconnectMinMs;
  _cfg.mqtt.reconnectMaxMs = doc["mqtt"]["reconnectMaxMs"] | _cfg.mqtt.reconnectMaxMs;
  _cfg.mqtt.publishHealth = doc["mqtt"]["publishHealth"] | _cfg.mqtt.publishHealth;
  _cfg.mqtt.healthIntervalMs = doc["mqtt"]["healthIntervalMs"] | _cfg.mqtt.healthIntervalMs;
  _cfg.mqtt.tls = doc["mqtt"]["tls"] | _cfg.mqtt.tls;
  _cfg.mqtt.user = String((const char*)(doc["mqtt"]["user"] | _cfg.mqtt.user.c_str()));
  _cfg.mqtt.pass = String((const char*)(doc["mqtt"]["pass"] | _cfg.mqtt.pass.c_str()));

  // REST
  _cfg.rest.port = doc["rest"]["port"] | _cfg.rest.port;

  // History
  _cfg.history.path = String((const char*)(doc["history"]["path"] | _cfg.history.path.c_str()));
  _cfg.history.flushIntervalMs = doc["history"]["flushIntervalMs"] | _cfg.history.flushIntervalMs;
  _cfg.history.retentionDays = doc["history"]["retentionDays"] | _cfg.history.retentionDays;

  // Logging
  _cfg.logging.consoleLevel = String((const char*)(doc["logging"]["consoleLevel"] | _cfg.logging.consoleLevel.c_str()));
  _cfg.logging.sdLevel = String((const char*)(doc["logging"]["sdLevel"] | _cfg.logging.sdLevel.c_str()));
  _cfg.logging.sdEnabled = doc["logging"]["sdEnabled"] | _cfg.logging.sdEnabled;
  _cfg.logging.rotateDaily = doc["logging"]["rotateDaily"] | _cfg.logging.rotateDaily;
  _cfg.logging.retentionDays = doc["logging"]["retentionDays"] | _cfg.logging.retentionDays;

  // Watchdog
  _cfg.watchdog.enabled = doc["watchdog"]["enabled"] | _cfg.watchdog.enabled;
  _cfg.watchdog.timeoutMs = doc["watchdog"]["timeoutMs"] | _cfg.watchdog.timeoutMs;
  _cfg.watchdog.panicReset = doc["watchdog"]["panicReset"] | _cfg.watchdog.panicReset;

  // Security
  _cfg.security.restAuthMode = String((const char*)(doc["security"]["restAuthMode"] | _cfg.security.restAuthMode.c_str()));
  _cfg.security.allowAnonymousGet = doc["security"]["allowAnonymousGet"] | _cfg.security.allowAnonymousGet;
  _cfg.security.restUser = String((const char*)(doc["security"]["restUser"] | _cfg.security.restUser.c_str()));
  _cfg.security.restPass = String((const char*)(doc["security"]["restPass"] | _cfg.security.restPass.c_str()));
  _cfg.security.restToken = String((const char*)(doc["security"]["restToken"] | _cfg.security.restToken.c_str()));
  _cfg.security.mqttUser = String((const char*)(doc["security"]["mqttUser"] | _cfg.security.mqttUser.c_str()));
  _cfg.security.mqttPass = String((const char*)(doc["security"]["mqttPass"] | _cfg.security.mqttPass.c_str()));

  return true;
}

bool ConfigManager::validate() {
  bool ok = true;

  if (!_cfg.network.hostname.length()) {
    _log->error("config: network.hostname is required");
    ok = false;
  }

  if (_cfg.sensors.resolutionBits < 9 || _cfg.sensors.resolutionBits > 12) {
    _log->warn("config: sensors.resolutionBits out of range (9..12), forcing 12");
    _cfg.sensors.resolutionBits = 12;
  }

  if (_cfg.sensors.intervalMs < 500) {
    _log->warn("config: sensors.intervalMs too low, forcing 500ms");
    _cfg.sensors.intervalMs = 500;
  }

  if (_cfg.sensors.conversionTimeoutMs < 50) {
    _log->warn("config: sensors.conversionTimeoutMs too low, forcing 50ms");
    _cfg.sensors.conversionTimeoutMs = 50;
  }

  if (_cfg.mqtt.reconnectMinMs < 250) _cfg.mqtt.reconnectMinMs = 250;
  if (_cfg.mqtt.reconnectMaxMs < _cfg.mqtt.reconnectMinMs) _cfg.mqtt.reconnectMaxMs = _cfg.mqtt.reconnectMinMs;
  if (_cfg.mqtt.healthIntervalMs == 0) _cfg.mqtt.healthIntervalMs = 15000;
  if (_cfg.mqtt.healthIntervalMs < 1000) {
    _log->warn("config: mqtt.healthIntervalMs too low, forcing 1000ms");
    _cfg.mqtt.healthIntervalMs = 1000;
  }

  if (_cfg.mqtt.enabled && _cfg.mqtt.tls) {
    _log->warn("config: mqtt.tls=true requested but unsupported by current MQTT client build. MQTT will be disabled to avoid insecure fallback.");
    _cfg.mqtt.enabled = false;
  }

  if (_cfg.rest.enabled && _cfg.rest.port == 0) {
    _log->error("config: rest.port must be > 0 when rest.enabled=true");
    ok = false;
  }

  if (_cfg.mqtt.enabled) {
    if (!_cfg.mqtt.host.length()) {
      _log->error("config: mqtt.host is required when mqtt.enabled=true");
      ok = false;
    }
    if (_cfg.mqtt.port == 0) {
      _log->error("config: mqtt.port must be > 0 when mqtt.enabled=true");
      ok = false;
    }
    if (!_cfg.mqtt.baseTopic.length()) {
      _log->error("config: mqtt.baseTopic is required when mqtt.enabled=true");
      ok = false;
    }
  }

  if (_cfg.ota.healthConfirmMs < 5000) {
    _cfg.ota.healthConfirmMs = 5000;
  }

  if (_cfg.history.flushIntervalMs > 60000) {
    _log->warn("config: history.flushIntervalMs too high, forcing 60000ms");
    _cfg.history.flushIntervalMs = 60000;
  }

  if (_cfg.history.retentionDays > 3650) {
    _log->warn("config: history.retentionDays too high, forcing 3650 days");
    _cfg.history.retentionDays = 3650;
  }

  if (!normalizeLogLevelName(_cfg.logging.consoleLevel)) {
    _log->error("config: logging.consoleLevel invalid (allowed: DEBUG|INFO|WARN|ERROR)");
    ok = false;
  }

  if (!normalizeLogLevelName(_cfg.logging.sdLevel)) {
    _log->error("config: logging.sdLevel invalid (allowed: DEBUG|INFO|WARN|ERROR)");
    ok = false;
  }

  if (_cfg.history.enabled && !_cfg.history.path.length()) {
    _log->error("config: history.path is required when history.enabled=true");
    ok = false;
  }

  if (_cfg.logging.retentionDays > 3650) {
    _log->warn("config: logging.retentionDays too high, forcing 3650 days");
    _cfg.logging.retentionDays = 3650;
  }

  if (_cfg.logging.retentionDays > 0 && !_cfg.logging.rotateDaily) {
    _log->warn("config: logging.retentionDays requires logging.rotateDaily=true; retention will be inactive");
  }

  String restMode = _cfg.security.restAuthMode;
  restMode.trim();
  restMode.toLowerCase();
  if (!restMode.length()) {
    restMode = _cfg.security.enabled ? "token" : "anonymous";
  }
  if (restMode != "anonymous" && restMode != "token") {
    _log->error("config: security.restAuthMode invalid (allowed: anonymous|token)");
    ok = false;
  }
  _cfg.security.restAuthMode = restMode;
  _cfg.security.enabled = (restMode == "token");

  if (_cfg.security.enabled) {
    if (!_cfg.security.restToken.length()) {
      _log->error("config: restAuthMode=token requires security.restToken");
      ok = false;
    }
  }

  if (_cfg.ota.enabled) {
    if (!_cfg.security.enabled || !_cfg.security.restToken.length()) {
      _log->warn("config: OTA enabled but token auth is not active. Set security.restAuthMode=token and security.restToken.");
    }
    if (!_cfg.ota.allowInsecureHttp) {
      _log->warn("config: OTA insecure HTTP not allowed. Set ota.allowInsecureHttp=true to enable OTA endpoint.");
    }
    if (!_cfg.ota.requireHashHeader) {
      _log->warn("config: ota.requireHashHeader=false lowers OTA integrity guarantees.");
    }
  }

  return ok;
}

bool ConfigManager::persistCurrentConfig(bool sdAvailable,
                                         bool* savedLittleFs,
                                         bool* savedSd,
                                         String* error) {
  if (savedLittleFs) *savedLittleFs = false;
  if (savedSd) *savedSd = false;
  if (error) *error = "";

  JsonDocument doc;
  configToJson(_cfg, doc);
  String out;
  if (serializeJson(doc, out) == 0) {
    if (error) *error = "failed to serialize config";
    return false;
  }

  if (!LittleFS.begin(false)) {
    if (error) *error = "LittleFS unavailable";
    return false;
  }
  if (!writeStringAtomic(LittleFS, "/config.json", out)) {
    if (error) *error = "failed to write /config.json to LittleFS";
    return false;
  }
  if (savedLittleFs) *savedLittleFs = true;

  if (!sdAvailable) return true;

  SemaphoreHandle_t sdMutex = sharedSdMutex();
  if (!sdMutex) {
    if (error) *error = "SD mutex unavailable";
    return false;
  }
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    if (error) *error = "SD mutex timeout";
    return false;
  }

  const bool hasSdConfig = SD.exists("/config.json");
  if (!hasSdConfig) {
    xSemaphoreGive(sdMutex);
    return true;
  }

  const bool sdWriteOk = writeStringAtomic(SD, "/config.json", out);
  xSemaphoreGive(sdMutex);

  if (!sdWriteOk) {
    if (error) *error = "saved to LittleFS, but failed to write /config.json to SD";
    return false;
  }

  if (savedSd) *savedSd = true;
  return true;
}

bool ConfigManager::updateSensorIntervalAndPersist(uint32_t requestedMs,
                                                   bool sdAvailable,
                                                   uint32_t* appliedMs,
                                                   bool* savedLittleFs,
                                                   bool* savedSd,
                                                   String* error) {
  uint32_t clamped = requestedMs;
  if (clamped < 500) clamped = 500;
  if (clamped > 3600000) clamped = 3600000;

  const uint32_t previousMs = _cfg.sensors.intervalMs;
  _cfg.sensors.intervalMs = clamped;

  bool littleFsOk = false;
  bool sdOk = false;
  const bool ok = persistCurrentConfig(sdAvailable, &littleFsOk, &sdOk, error);

  if (!ok && !littleFsOk) {
    _cfg.sensors.intervalMs = previousMs;
  }

  if (appliedMs) *appliedMs = _cfg.sensors.intervalMs;
  if (savedLittleFs) *savedLittleFs = littleFsOk;
  if (savedSd) *savedSd = sdOk;
  return ok;
}

bool ConfigManager::loadFromSources(bool sdAvailable, bool littleFsAvailable) {
  applyDefaults();

  bool loadedAny = false;

  if (littleFsAvailable) {
    if (LittleFS.exists("/config.json")) {
      String json;
      if (!readFileToString(LittleFS, "/config.json", json)) {
        _log->error("failed to read /config.json from LittleFS");
        return false;
      }
      _log->info("loading /config.json from LittleFS");
      if (!parseJson(json)) return false;
      loadedAny = true;
    } else {
      _log->warn("config: /config.json not found on LittleFS");
    }
  } else {
    _log->warn("config: LittleFS unavailable");
  }

  if (sdAvailable) {
    if (SD.exists("/config.json")) {
      String json;
      if (!readFileToString(SD, "/config.json", json)) {
        _log->error("failed to read /config.json from SD");
        return false;
      }
      _log->info("loading /config.json from SD (overrides LittleFS)");
      if (!parseJson(json)) return false;
      loadedAny = true;
    } else {
      _log->info("config: /config.json not found on SD");
    }
  }

  if (!loadedAny) {
    _log->error("FAIL-FAST: no /config.json found on LittleFS or SD.");
    return false;
  }

  const bool valid = validate();
  syncFeatureFlags();
  return valid;
}
