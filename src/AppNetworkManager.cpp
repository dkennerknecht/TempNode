#include "AppNetworkManager.h"

static uint8_t ethHost() {
#if defined(SPI3_HOST)
  return SPI3_HOST;
#elif defined(VSPI)
  return VSPI;
#elif defined(HSPI)
  return HSPI;
#else
  return 3;
#endif
}
#include "LogManager.h"
#include "TimeManager.h"
#include "Config.h"
#include "pins.h"

AppNetworkManager* AppNetworkManager::_self = nullptr;

void AppNetworkManager::begin(const AppConfig& cfg, LogManager& log, TimeManager& timeMgr) {
  _cfg = &cfg;
  _log = &log;
  _tm = &timeMgr;

  _self = this;
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    if (_self) _self->handleEvent(event);
  });

  _ethSpi = SPIClass(FSPI);
  _reconnectRequested = true;
  _nextRetryMs = 0;
}

void AppNetworkManager::requestReconnect() {
  _reconnectRequested = true;
  _nextRetryMs = millis();
}

String AppNetworkManager::macStr() const {
  uint8_t mac[6];
  ETH.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool AppNetworkManager::startEth() {
  if (_starting || _started) return _started;
  _starting = true;

  // Use ETH.begin overload with spi_host_device_t + explicit pins (Arduino 3.3.x / IDF 5.5)
  spi_host_device_t host;
#if defined(SPI3_HOST)
  host = SPI3_HOST;
#elif defined(VSPI)
  host = (spi_host_device_t)VSPI;
#elif defined(HSPI)
  host = (spi_host_device_t)HSPI;
#else
  host = (spi_host_device_t)3;
#endif

  const uint8_t freqMhz = 32; // safe default for W5500; can be lowered if needed
  bool ok = ETH.begin(ETH_PHY_W5500, 1, (int)PIN_ETH_CS, (int)PIN_ETH_INT, (int)PIN_ETH_RST,
                      host, (int)PIN_ETH_SCLK, (int)PIN_ETH_MISO, (int)PIN_ETH_MOSI, freqMhz);

  if (!ok) {
    _log->error("ETH.begin failed");
    _starting = false;
    _started = false;
    return false;
  }

  _started = true;
  _starting = false;
  _lastTryMs = millis();
  _log->info("ETH begin OK, waiting for link/DHCP...");
  return true;
}

void AppNetworkManager::stopEth() {
  if (!_started) return;
#if defined(ARDUINO_EVENT_ETH_START)
  // Arduino-ESP32 3.x
  ETH.end();
#else
  // no-op on older cores
#endif
  _started = false;
  _linkUp = false;
  _hasIp = false;
}

void AppNetworkManager::handleEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      _log->info("ETH start");
      ETH.setHostname(_cfg->network.hostname.c_str());
      if (_cfg->network.dhcp) {
        if (!ETH.config(IPAddress(), IPAddress(), IPAddress())) {
          _log->error("ETH DHCP config failed");
        }
      } else {
        IPAddress ip, gw, mask, dns;
        bool ok = ip.fromString(_cfg->network.ip) &&
                  gw.fromString(_cfg->network.gw) &&
                  mask.fromString(_cfg->network.mask);
        if (_cfg->network.dns.length()) {
          ok = ok && dns.fromString(_cfg->network.dns);
        } else {
          dns = gw;
        }
        if (!ok) {
          _log->error("ETH static IP config invalid, falling back to DHCP");
          if (!ETH.config(IPAddress(), IPAddress(), IPAddress())) {
            _log->error("ETH DHCP fallback config failed");
          }
        } else if (!ETH.config(ip, gw, mask, dns)) {
          _log->error("ETH static IP apply failed");
        } else {
          _log->info(String("ETH static IP configured: ") + ip.toString());
        }
      }
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      _linkUp = true;
      _log->info("ETH link up");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      _linkUp = false;
      _hasIp = false;
      _log->warn("ETH link down");
      requestReconnect();
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      _hasIp = true;
      _log->info(String("ETH got IP: ") + ETH.localIP().toString());
      _tm->requestNtpSync();
      break;
    case ARDUINO_EVENT_ETH_STOP:
      _linkUp = false;
      _hasIp = false;
      _log->warn("ETH stop");
      break;
    default:
      break;
  }
}

void AppNetworkManager::loop() {
  uint32_t now = millis();

  if (_reconnectRequested && (int32_t)(now - _nextRetryMs) >= 0) {
    _reconnectRequested = false;

    if (!_started) {
      if (startEth()) {
        _attempt = 0;
        _nextRetryMs = now + 1000;
        return;
      }
    } else {
      // soft restart if no ip for long
      stopEth();
      (void)startEth();
    }

    // backoff
    if (_attempt < 10) _attempt++;
    uint32_t backoff = 1000UL << (_attempt > 5 ? 5 : _attempt);
    if (backoff > 60000) backoff = 60000;
    _nextRetryMs = now + backoff;
    _reconnectRequested = true;
    _log->warn(String("ETH reconnect backoff ms=") + backoff);
  }
}
