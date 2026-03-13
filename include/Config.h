#pragma once
#include <Arduino.h>

struct SecurityConfig {
  bool enabled = false;
  // REST basic auth
  String restUser = "";
  String restPass = "";
  // MQTT auth
  String mqttUser = "";
  String mqttPass = "";
  // Optional bearer token for REST (if set and security.enabled)
  String restToken = "";
};

struct RestConfig {
  bool enabled = true;
  uint16_t port = 80;
};

struct MqttConfig {
  bool enabled = true;
  String host = "192.168.1.117";
  uint16_t port = 1883;
  bool tls = false;
  String clientId = "";
  String deviceId = "";
  String baseTopic = "device";
  uint32_t reconnectMinMs = 1000;
  uint32_t reconnectMaxMs = 60000;
  uint16_t offlineBufferPerSensor = 20; // ringbuffer size per sensor
};

struct OtaConfig {
  bool enabled = false;
  bool allowInsecureHttp = false;   // explicit opt-in: REST server is plain HTTP
  bool allowDowngrade = false;      // reject same/older app version by default
  bool requireHashHeader = true;    // require X-OTA-SHA256 or X-OTA-MD5
  uint32_t healthConfirmMs = 30000; // confirm new image after stable runtime
  bool requireNetworkForConfirm = true;
};

struct MetricsConfig {
  bool enabled = true;
};

struct HistoryConfig {
  bool enabled = true;
  String path = "/history.jsonl";
  uint32_t flushIntervalMs = 1000;
  uint32_t retentionDays = 0; // 0 = no retention
};

struct SensorConfig {
  uint32_t intervalMs = 5000;
  uint16_t resolutionBits = 12; // 9..12
  uint32_t conversionTimeoutMs = 1000; // max wait for DS18B20 conversion
};

struct WatchdogConfig {
  bool enabled = false;
  uint32_t timeoutMs = 15000;
  bool panicReset = true;
};

struct NetworkConfig {
  String hostname = "esp32-s3-eth";
  bool dhcp = true;
  // Static (only if dhcp=false)
  String ip = "192.168.1.50";
  String gw = "192.168.1.1";
  String mask = "255.255.255.0";
  String dns = "192.168.1.1";
};

struct AppConfig {
  NetworkConfig network;
  SecurityConfig security;
  RestConfig rest;
  MqttConfig mqtt;
  OtaConfig ota;
  MetricsConfig metrics;
  HistoryConfig history;
  SensorConfig sensors;
  WatchdogConfig watchdog;

  // Feature flags (redundant but explicit)
  bool restEnabled = true;
  bool mqttEnabled = true;
  bool securityEnabled = false;
  bool otaEnabled = false;
  bool metricsEnabled = true;
  bool historyEnabled = true;
};
