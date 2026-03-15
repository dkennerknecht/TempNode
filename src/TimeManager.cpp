#include "TimeManager.h"
#include "LogManager.h"
#include "StatsManager.h"
#include <time.h>

static constexpr time_t kMinValidEpochSec = 1700000000;            // ~2023-11
static constexpr uint32_t kPeriodicResyncMs = 6UL * 60UL * 60UL * 1000UL;

void TimeManager::_startSntp() {
  setenv("TZ", _tz.c_str(), 1);
  tzset();
  configTzTime(_tz.c_str(), "pool.ntp.org", "time.nist.gov");
}

void TimeManager::begin(const char* tz) {
  _bootUptimeMs = millis();
  _timeValid = false;
  _source = TimeSource::UPTIME;
  _tz = tz ? tz : "CET-1CEST,M3.5.0/2,M10.5.0/3";

  // NTP config (async)
  _startSntp();
  _ntpRequested = true;
  _ntpNextTryMs = 0;
  _nextPeriodicSyncMs = millis() + kPeriodicResyncMs;
  _ntpAttempts = 0;
}

void TimeManager::requestNtpSync() {
  _startSntp();
  _ntpRequested = true;
  _ntpNextTryMs = 0;
}

void TimeManager::_applyTimeValid() {
  _timeValid = true;
  _source = TimeSource::NTP;
  _lastSyncUptimeMs = millis();
}

void TimeManager::loop() {
  uint32_t nowMs = millis();

  if (_timeValid && (int32_t)(nowMs - _nextPeriodicSyncMs) >= 0) {
    requestNtpSync();
    _nextPeriodicSyncMs = nowMs + kPeriodicResyncMs;
  }

  if (!_ntpRequested) return;
  if ((int32_t)(nowMs - _ntpNextTryMs) < 0) return;

  time_t nowSec = 0;
  time(&nowSec);
  if (nowSec >= kMinValidEpochSec) {
    bool wasInvalid = !_timeValid;
    _applyTimeValid();
    _ntpRequested = false;
    _ntpAttempts = 0;
    _nextPeriodicSyncMs = nowMs + kPeriodicResyncMs;

    if (wasInvalid) {
      struct tm t;
      localtime_r(&nowSec, &t);
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
      if (_log) _log->info(String("NTP sync ok: ") + buf);
      else Serial.println(String("NTP sync ok: ") + buf);
    }
    return;
  }

  if (_stats) {
    _stats->incrementNtpSyncFails();
  }

  if (_ntpAttempts == 0 || (_ntpAttempts % 5) == 0) {
    _startSntp();
  }

  // backoff: 1s,2s,4s,8s.. max 60s
  if (_ntpAttempts < 10) _ntpAttempts++;
  uint32_t backoff = 1000UL << (_ntpAttempts > 5 ? 5 : _ntpAttempts); // cap at 32s
  if (backoff > 60000) backoff = 60000;
  _ntpNextTryMs = nowMs + backoff;
}

uint64_t TimeManager::uptimeMs() const {
  return (uint64_t)millis();
}

bool TimeManager::timeStale(uint32_t maxAgeMs) const {
  if (!_timeValid) return true;
  return (uint32_t)(millis() - _lastSyncUptimeMs) > maxAgeMs;
}

TimeStamp TimeManager::now() {
  TimeStamp ts;
  if (_timeValid) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    ts.epochMs = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    ts.millisPart = (uint32_t)(ts.epochMs % 1000ULL);
    ts.valid = true;
    ts.source = TimeSource::NTP;
    return ts;
  }

  ts.epochMs = (uint64_t)millis(); // uptime-based
  ts.millisPart = (uint32_t)(ts.epochMs % 1000ULL);
  ts.valid = false;
  ts.source = TimeSource::UPTIME;
  return ts;
}

String TimeManager::formatForLog(const TimeStamp& ts) const {
  char buf[32];
  if (ts.valid && ts.source == TimeSource::NTP) {
    time_t sec = (time_t)(ts.epochMs / 1000ULL);
    struct tm t;
    localtime_r(&sec, &t);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d,%03u",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (unsigned)ts.millisPart);
    return String(buf);
  }

  // UPTIME pseudo timestamp: 1970-01-01 00:00:00,mmm + uptime ms
  uint64_t ms = ts.epochMs;
  uint32_t mmm = (uint32_t)(ms % 1000ULL);
  uint64_t s = ms / 1000ULL;
  uint32_t ss = (uint32_t)(s % 60ULL);
  uint32_t mm = (uint32_t)((s / 60ULL) % 60ULL);
  uint32_t hh = (uint32_t)((s / 3600ULL) % 24ULL);
  snprintf(buf, sizeof(buf), "1970-01-01 %02u:%02u:%02u,%03u", hh, mm, ss, (unsigned)mmm);
  return String(buf);
}
