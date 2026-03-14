#pragma once
#include <Arduino.h>

struct SecurityConfig {
  bool enabled;
  // REST basic auth
  String restUser;
  String restPass;
  // MQTT auth
  String mqttUser;
  String mqttPass;
  // Optional bearer token for REST (if set and security.enabled)
  String restToken;
};

struct RestConfig {
  bool enabled;
  uint16_t port;
};

struct MqttConfig {
  bool enabled;
  String host;
  uint16_t port;
  bool tls;
  String user;
  String pass;
  String clientId;
  String deviceId;
  String baseTopic;
  uint32_t reconnectMinMs;
  uint32_t reconnectMaxMs;
  uint16_t offlineBufferPerSensor; // ringbuffer size per sensor
  bool publishHealth;
  uint32_t healthIntervalMs;
};

struct OtaConfig {
  bool enabled;
  bool allowInsecureHttp;   // explicit opt-in: REST server is plain HTTP
  bool allowDowngrade;      // reject same/older app version by default
  bool requireHashHeader;    // require X-OTA-SHA256 or X-OTA-MD5
  uint32_t healthConfirmMs; // confirm new image after stable runtime
  bool requireNetworkForConfirm;
};

struct MetricsConfig {
  bool enabled;
};

struct HistoryConfig {
  bool enabled;
  String path;
  uint32_t flushIntervalMs;
  uint32_t retentionDays; // 0 = no retention
};

struct LoggingConfig {
  String consoleLevel;
  String sdLevel;
  bool sdEnabled;
  bool rotateDaily;
  uint16_t retentionDays; // 0 = no retention
};

struct SensorConfig {
  uint32_t intervalMs;
  uint16_t resolutionBits; // 9..12
  uint32_t conversionTimeoutMs; // max wait for DS18B20 conversion
};

struct WatchdogConfig {
  bool enabled;
  uint32_t timeoutMs;
  bool panicReset;
};

struct NetworkConfig {
  String hostname;
  bool dhcp;
  // Static (only if dhcp=false)
  String ip;
  String gw;
  String mask;
  String dns;
};

struct AppConfig {
  NetworkConfig network;
  SecurityConfig security;
  RestConfig rest;
  MqttConfig mqtt;
  OtaConfig ota;
  MetricsConfig metrics;
  HistoryConfig history;
  LoggingConfig logging;
  SensorConfig sensors;
  WatchdogConfig watchdog;

  // Feature flags (redundant but explicit)
  bool restEnabled;
  bool mqttEnabled;
  bool securityEnabled;
  bool otaEnabled;
  bool metricsEnabled;
  bool historyEnabled;
};
