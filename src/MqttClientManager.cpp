#include "MqttClientManager.h"
#include "Config.h"
#include "AppNetworkManager.h"
#include "LogManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "SensorManager.h"
#include <ArduinoJson.h>

static const char* mqttDisconnectReasonName(AsyncMqttClientDisconnectReason reason) {
  switch (reason) {
    case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED: return "TCP_DISCONNECTED";
    case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION: return "MQTT_UNACCEPTABLE_PROTOCOL_VERSION";
    case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED: return "MQTT_IDENTIFIER_REJECTED";
    case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE: return "MQTT_SERVER_UNAVAILABLE";
    case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS: return "MQTT_MALFORMED_CREDENTIALS";
    case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED: return "MQTT_NOT_AUTHORIZED";
    case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE: return "ESP8266_NOT_ENOUGH_SPACE";
    case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT: return "TLS_BAD_FINGERPRINT";
    default: return "UNKNOWN";
  }
}

void MqttClientManager::begin(const AppConfig& cfg, AppNetworkManager& net, LogManager& log, StatsManager& stats, TimeManager& tm) {
  _cfg = &cfg;
  _net = &net;
  _log = &log;
  _stats = &stats;
  _tm = &tm;

  _deviceId = cfg.mqtt.deviceId.length() ? cfg.mqtt.deviceId : String("esp32s3-") + _net->macStr();
  _base = cfg.mqtt.baseTopic.length() ? cfg.mqtt.baseTopic : "device";

  // Keep all strings alive (AsyncMqttClient may keep pointers)
  _tls = cfg.mqtt.tls;
  _host = cfg.mqtt.host;
  if (cfg.mqtt.clientId.length()) {
    _clientId = cfg.mqtt.clientId;
  } else {
    String macNoSep = _net->macStr();
    macNoSep.replace(":", "");
    _clientId = String("esp32s3-") + macNoSep;
  }

  if (cfg.mqtt.user.length() || cfg.mqtt.pass.length()) {
    _user = cfg.mqtt.user;
    _pass = cfg.mqtt.pass;
  } else if (cfg.security.mqttUser.length() || cfg.security.mqttPass.length()) {
    _log->warn("config: security.mqttUser/security.mqttPass are legacy, prefer mqtt.user/mqtt.pass");
    _user = cfg.security.mqttUser;
    _pass = cfg.security.mqttPass;
  } else {
    _user = "";
    _pass = "";
  }

  if (_tls) {
    _log->error("MQTT TLS requested, but this AsyncMqttClient build has no TLS support. MQTT stays disabled (no plaintext fallback).");
    return;
  }
  _mqtt.setClientId(_clientId.c_str());
  _mqtt.setServer(_host.c_str(), cfg.mqtt.port);
  _log->info(String("MQTT target: ") + _host + ":" + String(cfg.mqtt.port) + " clientId=" + _clientId);

  if (_user.length()) {
    _log->info("MQTT auth: username configured");
    _mqtt.setCredentials(_user.c_str(), _pass.c_str());
  } else if (_pass.length()) {
    _log->warn("config: mqtt.pass set but mqtt.user is empty; trying anonymous MQTT login");
  } else {
    _log->info("MQTT auth: no credentials configured, trying anonymous login");
  }

  _willTopic = topicStatus();
  _mqtt.setWill(_willTopic.c_str(), 0, true, "{\"status\":\"offline\"}");

  _mqtt.onConnect([this](bool sessionPresent) {
    (void)sessionPresent;
    _connected = true;
    _backoffMs = _cfg->mqtt.reconnectMinMs;
    _log->info("MQTT connected");
    publishStatus("online", true);
    flushBuffers();
  });

  _mqtt.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
    _connected = false;
    _stats->incrementMqttReconnects();
    String msg = String("MQTT disconnected, reason=") + mqttDisconnectReasonName(reason) + " (" + (int)reason + ")";
    _log->warn(msg);
    if (reason == AsyncMqttClientDisconnectReason::TCP_DISCONNECTED) {
      _log->warn("MQTT hint: TCP disconnected before MQTT CONNACK. Check mqtt.host/mqtt.port, broker listener type (plain 1883 vs TLS 8883), and broker-side ACL/firewall.");
    } else if (reason == AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED) {
      _log->warn("MQTT hint: broker rejected authentication. Configure mqtt.user/mqtt.pass or enable anonymous access on broker.");
    } else if (reason == AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS) {
      _log->warn("MQTT hint: malformed credentials or username/password flags mismatch.");
    } else if (reason == AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED) {
      _log->warn("MQTT hint: client ID rejected by broker. Set mqtt.clientId to a simple unique ASCII value.");
    }
    scheduleReconnect();
  });

  _mqtt.onPublish([this](uint16_t packetId) {
    (void)packetId;
  });

  scheduleReconnect();
}

void MqttClientManager::scheduleReconnect() {
  uint32_t now = millis();
  uint32_t minMs = _cfg->mqtt.reconnectMinMs;
  uint32_t maxMs = _cfg->mqtt.reconnectMaxMs;

  if (_backoffMs < minMs) _backoffMs = minMs;
  if (_backoffMs > maxMs) _backoffMs = maxMs;

  _nextReconnectMs = now + _backoffMs;
  // exp backoff
  uint64_t next = (uint64_t)_backoffMs * 2ULL;
  _backoffMs = (next > maxMs) ? maxMs : (uint32_t)next;
}

String MqttClientManager::topicTemps(const String& sensorId) const {
  return _base + "/" + _deviceId + "/temps/" + sensorId;
}

String MqttClientManager::topicSystem() const {
  return _base + "/" + _deviceId + "/system";
}

String MqttClientManager::topicStatus() const {
  return _base + "/" + _deviceId + "/status";
}

String MqttClientManager::readingToJson(const SensorReading& r) const {
  JsonDocument doc;
  doc["timestamp"] = (uint64_t)r.timestampMs;
  doc["value"] = r.tempC;
  doc["unit"] = "C";
  doc["sensorId"] = r.id;
  doc["status"] = (uint8_t)r.status;
  doc["timeSource"] = r.timeSource;
  doc["timeValid"] = r.timeValid;

  String out;
  serializeJson(doc, out);
  return out;
}

String MqttClientManager::systemToJson() const {
  JsonDocument doc;
  doc["timestamp"] = (uint64_t)_tm->now().epochMs;
  doc["ip"] = _net->ip().toString();
  doc["link"] = _net->linkUp();
  doc["mqtt"] = _connected;
  doc["heap"] = ESP.getFreeHeap();
  doc["uptimeMs"] = (uint64_t)_tm->uptimeMs();
  String out;
  serializeJson(doc, out);
  return out;
}

void MqttClientManager::ensureRing(const String& sensorId) {
  if (_rings.find(sensorId) != _rings.end()) return;
  Ring r;
  r.buf.resize(_cfg->mqtt.offlineBufferPerSensor);
  r.head = 0;
  r.count = 0;
  _rings.emplace(sensorId, std::move(r));
}

void MqttClientManager::ringPush(const String& sensorId, const BufferedMsg& m) {
  ensureRing(sensorId);
  Ring& r = _rings[sensorId];
  if (r.buf.empty()) return;

  r.buf[r.head] = m;
  r.head = (r.head + 1) % r.buf.size();
  if (r.count < r.buf.size()) r.count++;
}

void MqttClientManager::flushBuffers() {
  for (auto& kv : _rings) {
    Ring& r = kv.second;
    if (r.count == 0) continue;

    size_t size = r.buf.size();
    size_t start = (r.head + size - r.count) % size;

    for (size_t i = 0; i < r.count; i++) {
      size_t idx = (start + i) % size;
      BufferedMsg& m = r.buf[idx];
      if (!m.topic.length()) continue;
      _mqtt.publish(m.topic.c_str(), m.qos, m.retain, m.payload.c_str(), m.payload.length());
      m.topic = "";
      m.payload = "";
    }
    r.count = 0;
  }
}

void MqttClientManager::publishStatus(const char* status, bool retain) {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;
  if (!_connected) return;

  JsonDocument doc;
  doc["timestamp"] = (uint64_t)_tm->now().epochMs;
  doc["status"] = status;
  doc["ip"] = _net->ip().toString();
  doc["link"] = _net->linkUp();
  String out;
  serializeJson(doc, out);

  _mqtt.publish(topicStatus().c_str(), 0, retain, out.c_str(), out.length());
}

void MqttClientManager::publishSensor(const SensorReading& r) {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;

  BufferedMsg m;
  m.topic = topicTemps(r.id);
  m.payload = readingToJson(r);
  m.retain = true;
  m.qos = 0;

  if (_connected) {
    _mqtt.publish(m.topic.c_str(), m.qos, m.retain, m.payload.c_str(), m.payload.length());
  } else {
    ringPush(r.id, m);
  }
}

void MqttClientManager::publishSystem() {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;
  if (!_connected) return;
  String out = systemToJson();
  _mqtt.publish(topicSystem().c_str(), 0, true, out.c_str(), out.length());
}

void MqttClientManager::loop() {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;

  uint32_t now = millis();
  if (!_connected) {
    if (_net->isUp() && (int32_t)(now - _nextReconnectMs) >= 0) {
      _log->info("MQTT connecting...");
      _mqtt.connect();
      scheduleReconnect();
    }
  }
}
