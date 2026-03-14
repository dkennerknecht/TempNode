#include "RestServer.h"
#include "Config.h"
#include "LogManager.h"
#include "TimeManager.h"
#include "AppNetworkManager.h"
#include "SensorManager.h"
#include "HistoryManager.h"
#include "StatsManager.h"
#include "MqttClientManager.h"
#include "TempNodeCore.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <Update.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <cstring>

static const char* timeSourceStr(uint8_t src) {
  return (src == 0) ? "NTP" : "UPTIME";
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

static String bytesToHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

void RestServer::resetOtaState() {
  if (_otaSha256Active) {
    mbedtls_sha256_free(&_otaSha256Ctx);
    _otaSha256Active = false;
  }
  _otaRejected = false;
  _otaVersionChecked = false;
  _otaExpectedBytes = 0;
  _otaReceivedBytes = 0;
  _otaHeaderBytes = 0;
  _otaMd5ExpectedSet = false;
  _otaSha256ExpectedSet = false;
  _otaRejectReason = "";
  _otaHeaderBuf.fill(0);
  _otaSha256Expected.fill(0);
}

bool RestServer::otaPrecheck(AsyncWebServerRequest* req, const String& filename) {
  if (!_cfg->security.enabled || !_cfg->security.restToken.length()) {
    _otaRejectReason = "OTA requires security.enabled=true and restToken";
    return false;
  }
  if (!req->hasHeader("Authorization")) {
    _otaRejectReason = "OTA requires Authorization: Bearer <token>";
    return false;
  } else {
    String auth = req->getHeader("Authorization")->value();
    if (!auth.startsWith("Bearer ")) {
      _otaRejectReason = "OTA requires Bearer token auth";
      return false;
    }
  }
  if (!_cfg->ota.allowInsecureHttp) {
    _otaRejectReason = "OTA over plain HTTP disabled (set ota.allowInsecureHttp=true)";
    return false;
  }
  if (!filename.endsWith(".bin")) {
    _otaRejectReason = "firmware filename must end with .bin";
    return false;
  }

  size_t contentLen = req->contentLength();
  if (contentLen == 0) {
    _otaRejectReason = "missing Content-Length";
    return false;
  }

  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    _otaRejectReason = "no OTA target partition";
    return false;
  }
  if (contentLen > target->size) {
    _otaRejectReason = "firmware too large for OTA partition";
    return false;
  }

  bool hashHeaderProvided = false;

  // Optional legacy integrity check
  if (req->hasHeader("X-OTA-MD5")) {
    String md5 = req->getHeader("X-OTA-MD5")->value();
    if (!Update.setMD5(md5.c_str())) {
      _otaRejectReason = "invalid X-OTA-MD5 header";
      return false;
    }
    _otaMd5ExpectedSet = true;
    hashHeaderProvided = true;
  }

  // Recommended integrity check
  if (req->hasHeader("X-OTA-SHA256")) {
    String sha256 = req->getHeader("X-OTA-SHA256")->value();
    if (!tempnode::parseHexDigest(sha256.c_str(), _otaSha256Expected.data(), _otaSha256Expected.size())) {
      _otaRejectReason = "invalid X-OTA-SHA256 header";
      return false;
    }

    mbedtls_sha256_init(&_otaSha256Ctx);
    mbedtls_sha256_starts(&_otaSha256Ctx, 0);
    _otaSha256Active = true;
    _otaSha256ExpectedSet = true;
    hashHeaderProvided = true;
  }

  if (_cfg->ota.requireHashHeader && !hashHeaderProvided) {
    _otaRejectReason = "missing OTA hash header (X-OTA-SHA256 or X-OTA-MD5)";
    return false;
  }

  _otaExpectedBytes = contentLen;
  _otaReceivedBytes = 0;
  _otaHeaderBytes = 0;
  _otaVersionChecked = false;

  if (!Update.begin(contentLen, U_FLASH, -1, LOW, target->label)) {
    _otaRejectReason = String("Update.begin failed: ") + Update.errorString();
    return false;
  }

  String integrity = _otaSha256ExpectedSet ? "sha256" : (_otaMd5ExpectedSet ? "md5" : "none");
  _log->warn(String("OTA precheck ok: target=") + target->label + " size=" + String((unsigned)contentLen) + " hash=" + integrity);
  return true;
}

bool RestServer::otaCheckVersionChunk(const uint8_t* data, size_t len) {
  constexpr size_t kDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  constexpr size_t kMinHeader = kDescOffset + sizeof(esp_app_desc_t);

  if (_otaVersionChecked) return true;

  size_t remain = (_otaHeaderBytes < _otaHeaderBuf.size()) ? (_otaHeaderBuf.size() - _otaHeaderBytes) : 0;
  size_t toCopy = (len < remain) ? len : remain;
  if (toCopy > 0) {
    memcpy(_otaHeaderBuf.data() + _otaHeaderBytes, data, toCopy);
    _otaHeaderBytes += toCopy;
  }

  if (_otaHeaderBytes < kMinHeader) return true;

  esp_app_desc_t incomingDesc{};
  memcpy(&incomingDesc, _otaHeaderBuf.data() + kDescOffset, sizeof(esp_app_desc_t));

  char newVer[33] = {0};
  char curVer[33] = {0};
  memcpy(newVer, incomingDesc.version, sizeof(incomingDesc.version));

  const esp_app_desc_t* running = esp_app_get_description();
  if (running) {
    memcpy(curVer, running->version, sizeof(running->version));
  }

  _otaVersionChecked = true;

  if (!newVer[0]) {
    _otaRejectReason = "incoming firmware has empty version";
    return false;
  }

  if (!_cfg->ota.allowDowngrade) {
    int cmp = tempnode::compareVersionStrings(newVer, curVer);
    if (cmp <= 0) {
      _otaRejectReason = String("incoming version ") + newVer + " <= current " + curVer;
      return false;
    }
  }

  _log->info(String("OTA version check ok: current=") + curVer + " incoming=" + newVer);
  return true;
}

bool RestServer::authOk(AsyncWebServerRequest* req) const {
  if (!_cfg->security.enabled) return true;

  // Token (Authorization: Bearer <token>)
  if (_cfg->security.restToken.length()) {
    if (req->hasHeader("Authorization")) {
      const AsyncWebHeader* h = req->getHeader("Authorization");
      String v = h->value();
      if (v.startsWith("Bearer ")) {
        String t = v.substring(7);
        if (t == _cfg->security.restToken) return true;
      }
    }
  }

  // Basic auth
  if (_cfg->security.restUser.length() && _cfg->security.restPass.length()) {
    if (req->authenticate(_cfg->security.restUser.c_str(), _cfg->security.restPass.c_str())) return true;
  }
  req->requestAuthentication();
  return false;
}

static void sendJson(AsyncWebServerRequest* req, JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

static void sendError(AsyncWebServerRequest* req, int code, const char* msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(req, doc, code);
}

void RestServer::begin(const AppConfig& cfg,
                       LogManager& log,
                       TimeManager& tm,
                       AppNetworkManager& net,
                       SensorManager& sensors,
                       HistoryManager& history,
                       StatsManager& stats,
                       MqttClientManager& mqtt) {
  _cfg = &cfg;
  _log = &log;
  _tm = &tm;
  _net = &net;
  _sensors = &sensors;
  _history = &history;
  _stats = &stats;
  _mqtt = &mqtt;

  if (!cfg.rest.enabled) return;

  _srv = new AsyncWebServer(cfg.rest.port);
  setupRoutes();
  _srv->begin();
  _log->info(String("REST started on port ") + cfg.rest.port);
}

void RestServer::setupRoutes() {
  _srv->on("/api/v1/health", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;

    PersistedStats st = _stats->snapshot();
    TimeStamp ts = _tm->now();

    const bool networkOk = _net->isUp();

    const bool mqttRequired = _cfg->mqtt.enabled;
    const bool mqttConnected = _mqtt->connected();
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
    mqtt["host"] = _cfg->mqtt.host;
    mqtt["port"] = _cfg->mqtt.port;
    mqtt["user"] = _mqtt->authUserLabel();
    mqtt["connectAttempts"] = _mqtt->connectAttempts();
    mqtt["diagLastUptimeMs"] = (uint64_t)_mqtt->lastDiagUptimeMs();
    mqtt["resolveOk"] = _mqtt->lastResolveOk();
    mqtt["resolvedIp"] = _mqtt->lastResolvedIp();
    mqtt["pingOk"] = _mqtt->lastPingOk();
    mqtt["pingMs"] = (uint64_t)_mqtt->lastPingMs();
    mqtt["tcpProbePort"] = _mqtt->lastTcpProbePort();
    mqtt["tcpProbeOpen"] = _mqtt->lastTcpProbeOpen();
    mqtt["altPortChecked"] = _mqtt->lastAltPortChecked();
    mqtt["altPort"] = _mqtt->lastAltPort();
    mqtt["altPortOpen"] = _mqtt->lastAltPortOpen();

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

    sendJson(req, doc, ok ? 200 : 503);
  });

  _srv->on("/api/v1/temps", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (auto& id : _sensors->sensorIds()) {
      SensorReading r;
      if (_sensors->getLatest(id, r)) {
        JsonObject o = arr.add<JsonObject>();
        o["sensorId"] = r.id;
        o["tempC"] = r.tempC;
        o["status"] = (uint8_t)r.status;
        o["timestamp"] = (uint64_t)r.timestampMs;
        o["timeValid"] = r.timeValid;
        o["timeSource"] = timeSourceStr(r.timeSource);
      }
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
  // Sensors summary (includes interval + latest readings)
  _srv->on("/api/v1/sensors", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    JsonDocument doc;
    doc["intervalMs"] = _sensors ? _sensors->intervalMs() : 0;
    JsonArray arr = doc["sensors"].to<JsonArray>();
    if (_sensors) {
      for (const auto& r : _sensors->latest()) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = r.id;
        if (isnan(r.tempC)) o["tempC"] = nullptr;
        else o["tempC"] = r.tempC;
        o["status"] = (uint8_t)r.status;
        o["timestampMs"] = r.timestampMs;
      }
    }
    sendJson(req, doc);
  });

  // Read sensor poll interval at runtime
  _srv->on("/api/v1/sensors/interval", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!_sensors) return sendError(req, 500, "sensors unavailable");

    JsonDocument doc;
    doc["intervalMs"] = _sensors->intervalMs();

    JsonArray arr = doc["sensors"].to<JsonArray>();
    for (const auto& r : _sensors->latest()) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = r.id;
      if (isnan(r.tempC)) o["tempC"] = nullptr;
      else o["tempC"] = r.tempC;
      o["status"] = (int)r.status;
      o["timestampMs"] = r.timestampMs;
    }
    sendJson(req, doc);
  });


  // Sensors interval write endpoint (form, JSON body, or query)
  auto setIntervalHandler = [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!_sensors) return sendError(req, 500, "sensors unavailable");

    uint32_t ms = 0;

    // query string or form-url-encoded: intervalMs=5000
    if (req->hasParam("intervalMs")) {
      const AsyncWebParameter* p = req->getParam("intervalMs");
      ms = (uint32_t)p->value().toInt();
    }
    if (ms == 0 && req->hasParam("intervalMs", true)) {
      const AsyncWebParameter* p = req->getParam("intervalMs", true);
      ms = (uint32_t)p->value().toInt();
    }

    // JSON body: {"intervalMs":5000}
    if (ms == 0 && req->hasParam("plain", true)) {
      const AsyncWebParameter* p = req->getParam("plain", true);
      JsonDocument body;
      if (deserializeJson(body, p->value()) == DeserializationError::Ok) {
        ms = body["intervalMs"] | 0;
      }
    }

    if (ms == 0) return sendError(req, 400, "missing intervalMs");

    _sensors->setIntervalMs(ms);

    JsonDocument doc;
    doc["intervalMs"] = _sensors->intervalMs();
    doc["ok"] = true;
    sendJson(req, doc);
  };

  _srv->on("/api/v1/sensors/interval", HTTP_POST, setIntervalHandler);
  _srv->on("/api/v1/sensors/interval", HTTP_PUT, setIntervalHandler);
  _srv->on("/api/v1/system", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    JsonDocument doc;
    PersistedStats st = _stats->snapshot();
    auto ts = _tm->now();
    doc["timestamp"] = (uint64_t)ts.epochMs;
    doc["timeValid"] = ts.valid;
    doc["timeSource"] = (ts.source == TimeSource::NTP) ? "NTP" : "UPTIME";
    doc["uptimeMs"] = (uint64_t)_tm->uptimeMs();
    doc["heap"] = ESP.getFreeHeap();
    doc["chipModel"] = ESP.getChipModel();
    doc["chipRev"] = ESP.getChipRevision();
    doc["sdk"] = ESP.getSdkVersion();
    doc["ip"] = _net->ip().toString();
    doc["link"] = _net->linkUp();
    doc["mac"] = _net->macStr();
    doc["resetReason"] = st.lastResetReason;
    doc["bootCount"] = st.bootCount;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  _srv->on("/api/v1/metrics", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!_cfg->metrics.enabled) {
      req->send(404, "application/json", "{\"error\":\"metrics disabled\"}");
      return;
    }

    JsonDocument doc;
    PersistedStats stSnap = _stats->snapshot();
    doc["heap"] = ESP.getFreeHeap();
    doc["uptimeMs"] = (uint64_t)_tm->uptimeMs();
    doc["link"] = _net->linkUp();
    doc["ip"] = _net->ip().toString();
    doc["mqttConnected"] = _mqtt->connected();
    doc["logQueueDepth"] = _log->queueDepth();
    doc["logDrops"] = _log->drops();

    JsonObject st = doc["stats"].to<JsonObject>();
    st["sdMountFails"] = stSnap.sdMountFails;
    st["sdWriteFails"] = stSnap.sdWriteFails;
    st["mqttReconnects"] = stSnap.mqttReconnects;
    st["sensorReadErrors"] = stSnap.sensorReadErrors;
    st["ntpSyncFails"] = stSnap.ntpSyncFails;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  _srv->on("/api/v1/history", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!_cfg->history.enabled) {
      req->send(404, "application/json", "{\"error\":\"history disabled\"}");
      return;
    }

    String sensorId = "";
    uint16_t limit = 100;

    if (req->hasParam("sensor")) sensorId = req->getParam("sensor")->value();
    if (req->hasParam("limit")) {
      limit = (uint16_t)req->getParam("limit")->value().toInt();
      if (limit == 0 || limit > 1000) limit = 100;
    }

    String out, err;
    if (!_history->readLastLines(sensorId, limit, out, err)) {
      JsonDocument doc;
      doc["error"] = err;
      String js;
      serializeJson(doc, js);
      req->send(503, "application/json", js);
      return;
    }
    req->send(200, "application/json", out);
  });

  // OTA (HTTP upload) - optional
  if (_cfg->ota.enabled) {
    if (!_cfg->security.enabled || !_cfg->security.restToken.length()) {
      _log->error("OTA endpoint disabled: requires security.enabled=true and security.restToken");
    } else if (!_cfg->ota.allowInsecureHttp) {
      _log->warn("OTA endpoint disabled: plain HTTP blocked (set ota.allowInsecureHttp=true to allow)");
    } else {
      _srv->on("/api/v1/ota", HTTP_POST,
        [this](AsyncWebServerRequest* req) {
          if (!authOk(req)) return;

          bool ok = false;
          String err = _otaRejectReason;

          if (!_otaRejected && !Update.hasError()) {
            if (_otaExpectedBytes == 0) {
              err = "OTA precheck not initialized";
            } else if (_otaReceivedBytes != _otaExpectedBytes) {
              err = String("OTA size mismatch: got ") + (unsigned)_otaReceivedBytes + ", expected " + (unsigned)_otaExpectedBytes;
            } else {
              if (_otaSha256ExpectedSet && _otaSha256Active) {
                uint8_t computed[32] = {0};
                mbedtls_sha256_finish(&_otaSha256Ctx, computed);
                if (memcmp(computed, _otaSha256Expected.data(), _otaSha256Expected.size()) != 0) {
                  err = String("SHA-256 mismatch: expected ") + bytesToHex(_otaSha256Expected.data(), _otaSha256Expected.size()) +
                        ", got " + bytesToHex(computed, sizeof(computed));
                }
                mbedtls_sha256_free(&_otaSha256Ctx);
                _otaSha256Active = false;
              }

              if (!err.length()) {
                if (Update.end(false)) {
                  ok = true;
                } else {
                  err = String("Update.end failed: ") + Update.errorString();
                }
              }
            }
          } else if (!err.length()) {
            err = Update.hasError() ? String(Update.errorString()) : "upload rejected";
          }

          _log->setPaused(false);
          _history->setPaused(false);

          if (ok) {
            req->send(200, "application/json", "{\"status\":\"ok\",\"reboot\":true}");
            _log->warn("OTA done, rebooting");
            resetOtaState();
            ESP.restart();
            return;
          }

          if (Update.isRunning()) Update.abort();
          _log->error(String("OTA failed: ") + err);
          JsonDocument doc;
          doc["status"] = "error";
          doc["message"] = err;
          sendJson(req, doc, 400);
          resetOtaState();
        },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
          if (!authOk(req)) return;

          if (index == 0) {
            resetOtaState();
            _log->warn(String("OTA start: ") + filename);
            _log->setPaused(true);
            _history->setPaused(true);

            if (!otaPrecheck(req, filename)) {
              _otaRejected = true;
              _log->error(String("OTA precheck failed: ") + _otaRejectReason);
              return;
            }
          }

          if (_otaRejected) return;

          if (!otaCheckVersionChunk(data, len)) {
            _otaRejected = true;
            _log->error(String("OTA version check failed: ") + _otaRejectReason);
            if (Update.isRunning()) Update.abort();
            return;
          }

          if (_otaSha256Active) {
            mbedtls_sha256_update(&_otaSha256Ctx, data, len);
          }

          if (!Update.hasError()) {
            size_t written = Update.write(data, len);
            if (written != len) {
              _otaRejected = true;
              _otaRejectReason = String("Update.write failed: ") + Update.errorString();
              _log->error(_otaRejectReason);
              if (Update.isRunning()) Update.abort();
              return;
            }
            _otaReceivedBytes += len;
          }

          if (final && _otaExpectedBytes && _otaReceivedBytes != _otaExpectedBytes) {
            _otaRejected = true;
            _otaRejectReason = String("incomplete upload: got ") + (unsigned)_otaReceivedBytes + ", expected " + (unsigned)_otaExpectedBytes;
            _log->error(_otaRejectReason);
            if (Update.isRunning()) Update.abort();
          }
        }
      );
    }
  }

  _srv->onNotFound([this](AsyncWebServerRequest* req) {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
  });
}
