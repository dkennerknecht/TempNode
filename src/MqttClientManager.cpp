#include "MqttClientManager.h"
#include "Config.h"
#include "AppNetworkManager.h"
#include "LogManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "SensorManager.h"
#include <ArduinoJson.h>
#include <NetworkClient.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <ping/ping_sock.h>

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

static const char* otaImgStateStr(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID: return "VALID";
    case ESP_OTA_IMG_INVALID: return "INVALID";
    case ESP_OTA_IMG_ABORTED: return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED: return "UNDEFINED";
    default: return "UNKNOWN";
  }
}

struct PingOnceCtx {
  volatile bool done = false;
  volatile bool success = false;
  volatile uint32_t timeMs = 0;
};

static void onPingSuccess(esp_ping_handle_t hdl, void* args) {
  PingOnceCtx* ctx = static_cast<PingOnceCtx*>(args);
  if (!ctx) return;

  uint32_t rtt = 0;
  if (esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt)) == ESP_OK) {
    ctx->timeMs = rtt;
  }
  ctx->success = true;
}

static void onPingEnd(esp_ping_handle_t hdl, void* args) {
  PingOnceCtx* ctx = static_cast<PingOnceCtx*>(args);
  if (!ctx) return;

  uint32_t replies = 0;
  if (esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &replies, sizeof(replies)) == ESP_OK && replies > 0) {
    ctx->success = true;
  }
  ctx->done = true;
}

void MqttClientManager::begin(const AppConfig& cfg, AppNetworkManager& net, LogManager& log, StatsManager& stats, TimeManager& tm) {
  _cfg = &cfg;
  _net = &net;
  _log = &log;
  _stats = &stats;
  _tm = &tm;

  _deviceId = cfg.mqtt.deviceId.length() ? cfg.mqtt.deviceId : String("esp32s3-") + _net->macStr();
  _base = cfg.mqtt.baseTopic;

  // Keep all strings alive (AsyncMqttClient may keep pointers)
  _tls = cfg.mqtt.tls;
  _host = cfg.mqtt.host;
  _port = cfg.mqtt.port;
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
  _mqtt.setServer(_host.c_str(), _port);
  _log->info(String("MQTT target: ") + _host + ":" + String(_port) + " clientId=" + _clientId);

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
    _nextHealthPublishMs = millis();
    publishHealth();
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

String MqttClientManager::authUserLabel() const {
  if (_user.length()) return _user;
  return "<anonymous>";
}

bool MqttClientManager::resolveHost(IPAddress& out) {
  if (out.fromString(_host)) return true;
  return WiFi.hostByName(_host.c_str(), out);
}

bool MqttClientManager::pingIp(const IPAddress& ip, uint32_t timeoutMs, uint32_t& outRttMs) {
  outRttMs = 0;

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count = 1;
  cfg.timeout_ms = timeoutMs;
  cfg.interval_ms = 1000;
  IP_ADDR4(&cfg.target_addr, ip[0], ip[1], ip[2], ip[3]);

  PingOnceCtx ctx{};
  esp_ping_callbacks_t cbs = {};
  cbs.cb_args = &ctx;
  cbs.on_ping_success = onPingSuccess;
  cbs.on_ping_end = onPingEnd;

  esp_ping_handle_t hdl = nullptr;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || !hdl) {
    return false;
  }

  if (esp_ping_start(hdl) != ESP_OK) {
    esp_ping_delete_session(hdl);
    return false;
  }

  const uint32_t waitUntilMs = millis() + timeoutMs + 500;
  while (!ctx.done && (int32_t)(millis() - waitUntilMs) < 0) {
    delay(10);
  }

  esp_ping_stop(hdl);
  esp_ping_delete_session(hdl);

  outRttMs = ctx.timeMs;
  return ctx.success;
}

bool MqttClientManager::tcpProbe(const IPAddress& ip, uint16_t port, uint32_t timeoutMs) {
  NetworkClient c;
  const int ok = c.connect(ip, port, (int32_t)timeoutMs);
  c.stop();
  return ok == 1;
}

void MqttClientManager::runConnectivityDiagnostics() {
  _diagAtUptimeMs = millis();
  _diagResolveOk = false;
  _diagResolvedIp = "";
  _diagPingOk = false;
  _diagPingMs = 0;
  _diagTcpOpen = false;
  _diagTcpPort = _port;
  _diagAltChecked = false;
  _diagAltPort = 0;
  _diagAltOpen = false;

  IPAddress ip;
  if (!resolveHost(ip)) {
    _log->warn(String("MQTT diag: DNS/host resolve failed for host=") + _host);
    return;
  }

  _diagResolveOk = true;
  _diagResolvedIp = ip.toString();

  _diagPingOk = pingIp(ip, 1000, _diagPingMs);
  _diagTcpOpen = tcpProbe(ip, _diagTcpPort, 1000);

  if (_port == 1883 || _port == 8883) {
    _diagAltChecked = true;
    _diagAltPort = (_port == 1883) ? 8883 : 1883;
    _diagAltOpen = tcpProbe(ip, _diagAltPort, 1000);
  }

  String summary = String("MQTT diag: host=") + _host +
                   " resolvedIp=" + _diagResolvedIp +
                   " ping=" + (_diagPingOk ? String(_diagPingMs) + "ms" : String("fail")) +
                   " tcp/" + String(_diagTcpPort) + "=" + (_diagTcpOpen ? "open" : "closed");
  if (_diagAltChecked) {
    summary += String(" tcp/") + _diagAltPort + "=" + (_diagAltOpen ? "open" : "closed");
  }
  _log->info(summary);

  if (!_diagTcpOpen) {
    _log->warn(String("MQTT diag: configured port ") + _diagTcpPort + " appears closed or unreachable from device");
  }
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

String MqttClientManager::topicHealth() const {
  return _base + "/" + _deviceId + "/health";
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

String MqttClientManager::healthToJson() const {
  PersistedStats st = _stats->snapshot();
  TimeStamp ts = _tm->now();

  const bool networkOk = _net->isUp();

  const bool mqttRequired = _cfg->mqtt.enabled;
  const bool mqttConnected = _connected;
  const bool mqttOk = !mqttRequired || mqttConnected;

  const bool sdRequired = _cfg->history.enabled;
  const bool sdAvailable = _log->sdAvailable();
  const bool sdOk = !sdRequired || sdAvailable;
  uint64_t sdSizeBytes = 0;
  uint64_t sdUsedBytes = 0;
  uint64_t sdFreeBytes = 0;
  double sdUsagePercent = 0.0;
  bool sdCapacityKnown = false;
  if (sdAvailable) {
    sdSizeBytes = (uint64_t)SD.totalBytes();
    sdUsedBytes = (uint64_t)SD.usedBytes();
    if (sdSizeBytes > 0) {
      if (sdUsedBytes > sdSizeBytes) sdUsedBytes = sdSizeBytes;
      sdFreeBytes = sdSizeBytes - sdUsedBytes;
      sdUsagePercent = ((double)sdUsedBytes * 100.0) / (double)sdSizeBytes;
      sdCapacityKnown = true;
    }
  }

  constexpr uint32_t kTimeStaleThresholdMs = 6UL * 60UL * 60UL * 1000UL;
  const bool timeValid = _tm->timeValid();
  const bool timeStale = timeValid ? _tm->timeStale(kTimeStaleThresholdMs) : false;
  const bool timeOk = timeValid && !timeStale;

  const bool otaEnabled = _cfg->ota.enabled;
  const bool otaEndpointActive = otaEnabled &&
                                 _cfg->security.enabled &&
                                 _cfg->security.restToken.length() &&
                                 _cfg->ota.allowInsecureHttp;

  esp_ota_img_states_t imgState = ESP_OTA_IMG_UNDEFINED;
  bool imgStateKnown = false;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && esp_ota_get_state_partition(running, &imgState) == ESP_OK) {
    imgStateKnown = true;
  }
  const bool otaPendingVerify = imgStateKnown && (imgState == ESP_OTA_IMG_PENDING_VERIFY);
  const bool otaOk = !otaEnabled || otaEndpointActive;

  const bool ok = networkOk && mqttOk && sdOk && otaOk;

  JsonDocument doc;
  doc["status"] = ok ? "ok" : "degraded";
  doc["timestamp"] = (uint64_t)ts.epochMs;

  JsonObject checks = doc["checks"].to<JsonObject>();

  JsonObject network = checks["network"].to<JsonObject>();
  network["ok"] = networkOk;
  network["up"] = _net->isUp();
  network["link"] = _net->linkUp();
  network["ip"] = _net->ip().toString();
  network["mac"] = _net->macStr();

  JsonObject mqtt = checks["mqtt"].to<JsonObject>();
  mqtt["required"] = mqttRequired;
  mqtt["enabled"] = _cfg->mqtt.enabled;
  mqtt["connected"] = mqttConnected;
  mqtt["ok"] = mqttOk;
  mqtt["host"] = _host;
  mqtt["port"] = _port;
  mqtt["user"] = authUserLabel();
  mqtt["connectAttempts"] = _connectAttempts;
  mqtt["diagLastUptimeMs"] = (uint64_t)_diagAtUptimeMs;
  mqtt["resolveOk"] = _diagResolveOk;
  mqtt["resolvedIp"] = _diagResolvedIp;
  mqtt["pingOk"] = _diagPingOk;
  mqtt["pingMs"] = (uint64_t)_diagPingMs;
  mqtt["tcpProbePort"] = _diagTcpPort;
  mqtt["tcpProbeOpen"] = _diagTcpOpen;
  mqtt["altPortChecked"] = _diagAltChecked;
  mqtt["altPort"] = _diagAltPort;
  mqtt["altPortOpen"] = _diagAltOpen;

  JsonObject sd = checks["sd"].to<JsonObject>();
  sd["required"] = sdRequired;
  sd["available"] = sdAvailable;
  sd["ok"] = sdOk;
  sd["sdMountFails"] = st.sdMountFails;
  sd["sdWriteFails"] = st.sdWriteFails;
  sd["diskSizeBytes"] = sdSizeBytes;
  sd["diskUsedBytes"] = sdUsedBytes;
  sd["diskFreeBytes"] = sdFreeBytes;
  sd["usagePercent"] = sdUsagePercent;
  sd["capacityKnown"] = sdCapacityKnown;

  JsonObject time = checks["time"].to<JsonObject>();
  time["valid"] = timeValid;
  time["source"] = (ts.source == TimeSource::NTP) ? "NTP" : "UPTIME";
  time["stale"] = timeStale;
  time["ok"] = timeOk;
  time["lastNtpSyncUptimeMs"] = (uint64_t)_tm->lastNtpSyncMs();
  time["ntpSyncFails"] = st.ntpSyncFails;

  JsonObject otaState = checks["ota_state"].to<JsonObject>();
  otaState["enabled"] = otaEnabled;
  otaState["endpointActive"] = otaEndpointActive;
  otaState["pendingVerify"] = otaPendingVerify;
  otaState["requireHashHeader"] = _cfg->ota.requireHashHeader;
  otaState["allowDowngrade"] = _cfg->ota.allowDowngrade;
  otaState["healthConfirmMs"] = _cfg->ota.healthConfirmMs;
  otaState["ok"] = otaOk;
  otaState["imageStateKnown"] = imgStateKnown;
  otaState["imageState"] = otaImgStateStr(imgState);

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

void MqttClientManager::publishHealth() {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;
  if (!_cfg->mqtt.publishHealth) return;
  if (!_connected) return;
  String out = healthToJson();
  _mqtt.publish(topicHealth().c_str(), 0, true, out.c_str(), out.length());
}

void MqttClientManager::loop() {
  if (!_cfg) return;
  if (!_cfg->mqtt.enabled) return;

  uint32_t now = millis();
  if (!_connected) {
    if (_net->isUp() && (int32_t)(now - _nextReconnectMs) >= 0) {
      runConnectivityDiagnostics();
      _connectAttempts++;
      _log->info(String("MQTT connecting: host=") + _host +
                 " port=" + String(_port) +
                 " user=" + authUserLabel() +
                 " clientId=" + _clientId +
                 " attempt=" + String(_connectAttempts));
      _mqtt.connect();
      scheduleReconnect();
    }
  } else if (_cfg->mqtt.publishHealth && (int32_t)(now - _nextHealthPublishMs) >= 0) {
    publishHealth();
    _nextHealthPublishMs = now + _cfg->mqtt.healthIntervalMs;
  }
}
