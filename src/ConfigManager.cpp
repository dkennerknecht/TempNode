#include "ConfigManager.h"
#include "LogManager.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <WiFi.h>

static bool readFileToString(const char* path, String& out) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  out.reserve((size_t)f.size() + 1);
  while (f.available()) out += (char)f.read();
  f.close();
  return true;
}

void ConfigManager::begin(LogManager& log) {
  _log = &log;
  applyDefaults();
}

void ConfigManager::applyDefaults() {
  _cfg = AppConfig{};
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
  _cfg.mqtt.tls = doc["mqtt"]["tls"] | _cfg.mqtt.tls;

  // REST
  _cfg.rest.port = doc["rest"]["port"] | _cfg.rest.port;

  // History
  _cfg.history.path = String((const char*)(doc["history"]["path"] | _cfg.history.path.c_str()));
  _cfg.history.flushIntervalMs = doc["history"]["flushIntervalMs"] | _cfg.history.flushIntervalMs;
  _cfg.history.retentionDays = doc["history"]["retentionDays"] | _cfg.history.retentionDays;

  // Watchdog
  _cfg.watchdog.enabled = doc["watchdog"]["enabled"] | _cfg.watchdog.enabled;
  _cfg.watchdog.timeoutMs = doc["watchdog"]["timeoutMs"] | _cfg.watchdog.timeoutMs;
  _cfg.watchdog.panicReset = doc["watchdog"]["panicReset"] | _cfg.watchdog.panicReset;

  // Security
  _cfg.security.restUser = String((const char*)(doc["security"]["restUser"] | _cfg.security.restUser.c_str()));
  _cfg.security.restPass = String((const char*)(doc["security"]["restPass"] | _cfg.security.restPass.c_str()));
  _cfg.security.restToken = String((const char*)(doc["security"]["restToken"] | _cfg.security.restToken.c_str()));
  _cfg.security.mqttUser = String((const char*)(doc["security"]["mqttUser"] | _cfg.security.mqttUser.c_str()));
  _cfg.security.mqttPass = String((const char*)(doc["security"]["mqttPass"] | _cfg.security.mqttPass.c_str()));

  // Derived flags
  _cfg.restEnabled = _cfg.rest.enabled;
  _cfg.mqttEnabled = _cfg.mqtt.enabled;
  _cfg.securityEnabled = _cfg.security.enabled;
  _cfg.otaEnabled = _cfg.ota.enabled;
  _cfg.metricsEnabled = _cfg.metrics.enabled;
  _cfg.historyEnabled = _cfg.history.enabled;

  return validate();
}

bool ConfigManager::validate() {
  bool ok = true;

  if (_cfg.sensors.resolutionBits < 9 || _cfg.sensors.resolutionBits > 12) {
    _log->warn("config: sensors.resolutionBits out of range (9..12), forcing 12");
    _cfg.sensors.resolutionBits = 12;
    ok = false;
  }

  if (_cfg.sensors.intervalMs < 500) {
    _log->warn("config: sensors.intervalMs too low, forcing 500ms");
    _cfg.sensors.intervalMs = 500;
    ok = false;
  }

  if (_cfg.mqtt.reconnectMinMs < 250) _cfg.mqtt.reconnectMinMs = 250;
  if (_cfg.mqtt.reconnectMaxMs < _cfg.mqtt.reconnectMinMs) _cfg.mqtt.reconnectMaxMs = _cfg.mqtt.reconnectMinMs;

  if (_cfg.rest.port == 0) _cfg.rest.port = 80;

  if (_cfg.security.enabled) {
    const bool basicConfigured = _cfg.security.restUser.length() > 0 && _cfg.security.restPass.length() > 0;
    const bool tokenConfigured = _cfg.security.restToken.length() > 0;

    if (!basicConfigured && !tokenConfigured) {
      _log->error("config: security enabled, but no REST auth configured (set restToken or restUser/restPass)");
      ok = false;
    }

    if (basicConfigured && _cfg.security.restUser == "admin" && _cfg.security.restPass == "admin") {
      _log->error("config: weak REST credentials admin/admin are not allowed when security is enabled");
      ok = false;
    }
  }

  return ok;
}

bool ConfigManager::loadFromSd() {
  if (!SD.exists("/config.json")) {
    _log->info("config.json not found on SD, using defaults");
    return true;
  }
  String json;
  if (!readFileToString("/config.json", json)) {
    _log->error("failed to read /config.json");
    return false;
  }
  _log->info("loading /config.json");
  return parseJson(json);
}
