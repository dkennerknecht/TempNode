#pragma once
#include <Arduino.h>

class StatsManager;

enum class TimeSource : uint8_t { NTP = 0, UPTIME = 1 };

struct TimeStamp {
  uint64_t epochMs = 0;     // if NTP valid: Unix epoch ms; else uptime-based ms since boot (pseudo)
  uint32_t millisPart = 0;  // epochMs % 1000 (cached)
  bool valid = false;
  TimeSource source = TimeSource::UPTIME;
};

class TimeManager {
public:
  void begin(const char* tz = "CET-1CEST,M3.5.0/2,M10.5.0/3");
  void loop();
  void setStats(StatsManager* stats) { _stats = stats; }

  void requestNtpSync();
  bool timeValid() const { return _timeValid; }
  TimeSource timeSource() const { return _source; }

  TimeStamp now();
  String formatForLog(const TimeStamp& ts) const; // YYYY-MM-DD HH:MM:SS,mmm (localtime if NTP)
  uint64_t uptimeMs() const;

  uint64_t lastNtpSyncMs() const { return _lastSyncUptimeMs; }
  bool timeStale(uint32_t maxAgeMs) const;

private:
  bool _timeValid = false;
  TimeSource _source = TimeSource::UPTIME;
  uint64_t _bootUptimeMs = 0;
  uint64_t _lastSyncUptimeMs = 0;
  String _tz = "CET-1CEST,M3.5.0/2,M10.5.0/3";

  bool _ntpRequested = false;
  uint32_t _ntpNextTryMs = 0;
  uint32_t _nextPeriodicSyncMs = 0;
  uint8_t _ntpAttempts = 0;
  StatsManager* _stats = nullptr;

  void _startSntp();
  void _applyTimeValid();
};
