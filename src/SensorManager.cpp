#include "SensorManager.h"
#include "pins.h"
#include "LogManager.h"
#include "StatsManager.h"
#include "TimeManager.h"
#include "Config.h"

static bool isAllFF(const DeviceAddress a) {
  for (int i=0;i<8;i++) if (a[i] != 0xFF) return false;
  return true;
}

void SensorManager::begin(const SensorConfig& cfg, LogManager& log, StatsManager& stats, TimeManager& tm) {
  _cfg = &cfg;
  
  _intervalMs = cfg.intervalMs;
_log = &log;
  _stats = &stats;
  _tm = &tm;

  _ow = new OneWire((uint8_t)PIN_1W_DATA);
  _dt = new DallasTemperature(_ow);
  _dt->begin();
  _dt->setWaitForConversion(false);
  _dt->setCheckForConversion(true);

  uint8_t res = (uint8_t)cfg.resolutionBits;
  _dt->setResolution(res);

  _state = State::DISCOVER;
  _nextActionMs = 0;
  _idx = 0;
}

String SensorManager::addrToHex(const uint8_t* addr) {
  char buf[17];
  for (int i = 0; i < 8; i++) {
    snprintf(&buf[i * 2], 3, "%02X", addr[i]);
  }
  buf[16] = '\0';
  return String(buf);
}

bool SensorManager::isValidTemp(float c, SensorStatus& status) {
  if (c == DEVICE_DISCONNECTED_C) {
    status = SensorStatus::DISCONNECTED;
    return false;
  }
  if (fabs(c - 85.0f) < 0.001f) { // DS18B20 power-up reading
    status = SensorStatus::INVALID_85C;
    return false;
  }
  if (c < -55.0f || c > 125.0f) {
    status = SensorStatus::INVALID_RANGE;
    return false;
  }
  status = SensorStatus::OK;
  return true;
}

void SensorManager::discover() {
  _ids.clear();
  _addrs.clear();
  _latest.clear();

  int count = _dt->getDeviceCount();
  _log->info(String("1-Wire devices: ") + count);

  DeviceAddress addr;
  for (int i = 0; i < count; i++) {
    if (_dt->getAddress(addr, i)) {
      if (isAllFF(addr)) continue;
      String id = addrToHex(addr);
      _ids.push_back(id);
            std::array<uint8_t,8> a{};
      memcpy(a.data(), addr, 8);
      _addrs.push_back(a);
      SensorReading lr; lr.id = id; _latest.push_back(lr);
    }
  }

  if (_ids.empty()) _log->warn("no DS18B20 found");
}

void SensorManager::requestConversionAll() {
  _dt->requestTemperatures();
  _convStartMs = millis();
}

bool SensorManager::conversionReady() const {
  // DallasTemperature can check conversion, but on ESP32 it may still read busy.
  // We implement a timeout-based readiness. For 12-bit: 750ms typical.
  return (uint32_t)(millis() - _convStartMs) >= 10; // allow immediate poll; dt will check if enabled
}

void SensorManager::readAll() {
  auto ts = _tm->now();
  for (size_t i = 0; i < _addrs.size(); i++) {
    float c = _dt->getTempC(_addrs[i].data());

    SensorStatus st = SensorStatus::OK;
    if (!isValidTemp(c, st)) {
      _readErrors++;
      _stats->incrementSensorReadErrors();
    }

    SensorReading r;
    r.id = _ids[i];
    r.tempC = c;
    r.status = st;
    r.timestampMs = ts.epochMs;
    r.timeValid = ts.valid;
    r.timeSource = (ts.source == TimeSource::NTP) ? 0 : 1;

    _latest[i] = r;
    if (_cb) _cb(r);
  }
}

bool SensorManager::getLatest(const String& id, SensorReading& out) const {
  for (size_t i = 0; i < _ids.size(); i++) {
    if (_ids[i] == id) {
      out = _latest[i];
      return true;
    }
  }
  return false;
}

void SensorManager::loop() {
  uint32_t now = millis();
  if ((int32_t)(now - _nextActionMs) < 0) return;

  switch (_state) {
    case State::DISCOVER:
      discover();
      _state = State::REQUEST;
      _nextActionMs = now;
      break;

    case State::REQUEST:
      requestConversionAll();
      _state = State::WAIT;
      _nextActionMs = now + 5;
      break;

    case State::WAIT:
      if (_dt->isConversionComplete()) {
        _state = State::READ;
        _nextActionMs = now;
        break;
      }
      if ((uint32_t)(now - _convStartMs) > _cfg->conversionTimeoutMs) {
        _readErrors++;
        _stats->incrementSensorReadErrors();
        _log->warn("DS18B20 conversion timeout");
        _state = State::READ;
        _nextActionMs = now;
        break;
      }
      _nextActionMs = now + 10;
      break;

    case State::READ:
      readAll();
      _state = State::REQUEST;
      _nextActionMs = now + _intervalMs;
      break;
  }
}


void SensorManager::setIntervalMs(uint32_t ms) {
  // clamp to avoid silly values
  if (ms < 250) ms = 250;
  if (ms > 3600000) ms = 3600000;
  _intervalMs = ms;
  // schedule next action based on new interval
  _nextActionMs = millis() + _intervalMs;
}
