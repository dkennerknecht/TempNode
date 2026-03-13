#include "RestServer.h"
#include "Config.h"
#include "LogManager.h"
#include "TimeManager.h"
#include "AppNetworkManager.h"
#include "SensorManager.h"
#include "HistoryManager.h"
#include "StatsManager.h"
#include "MqttClientManager.h"
#include <ArduinoJson.h>
#include <Update.h>

static const char* timeSourceStr(uint8_t src) {
  return (src == 0) ? "NTP" : "UPTIME";
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
    bool ok = _net->isUp();
    if (_cfg->mqtt.enabled) ok = ok && _mqtt->connected();
    if (ok) {
      req->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      req->send(503, "application/json", "{\"status\":\"degraded\"}");
    }
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

  // Update sensor poll interval at runtime
    // Get or set sensors interval (query param):
  //   GET /api/v1/sensors/interval           -> returns current interval
  //   GET /api/v1/sensors/interval?intervalMs=10000 -> sets + returns
  
  // Sensors interval GET/SET via query string
  _srv->on("/api/v1/sensors/interval", HTTP_GET, [this](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!_sensors) return sendError(req, 500, "sensors unavailable");

    // ?intervalMs=10000
    if (req->hasParam("intervalMs")) {
      const AsyncWebParameter* p = req->getParam("intervalMs");
      uint32_t ms = (uint32_t)p->value().toInt();
      if (ms > 0) _sensors->setIntervalMs(ms);
    }

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


  // Sensors interval POST (form, json, or query)
  _srv->on("/api/v1/sensors/interval", HTTP_POST, [this](AsyncWebServerRequest* req) {
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
  });
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
    _srv->on("/api/v1/ota", HTTP_POST,
      [this](AsyncWebServerRequest* req) {
        if (!authOk(req)) return;
        bool ok = !Update.hasError();
        _log->setPaused(false);
        _history->setPaused(false);
        req->send(ok ? 200 : 500, "application/json", ok ? "{\"status\":\"ok\",\"reboot\":true}" : "{\"status\":\"error\"}");
        if (ok) {
          _log->warn("OTA done, rebooting");
          ESP.restart();
        }
      },
      [this](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!authOk(req)) return;

        if (index == 0) {
          _log->warn(String("OTA start: ") + filename);
          _log->setPaused(true);
          _history->setPaused(true);

          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            _log->error("Update.begin failed");
          }
        }

        if (!Update.hasError()) {
          if (Update.write(data, len) != len) {
            _log->error("Update.write failed");
          }
        }

        if (final) {
          if (Update.end(true)) {
            _log->warn("OTA upload complete");
          } else {
            _log->error("Update.end failed");
            _log->setPaused(false);
            _history->setPaused(false);
          }
        }
      }
    );
  }

  _srv->onNotFound([this](AsyncWebServerRequest* req) {
    req->send(404, "application/json", "{\"error\":\"not found\"}");
  });
}
