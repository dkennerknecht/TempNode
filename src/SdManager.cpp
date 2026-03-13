#include "SdManager.h"
#include "pins.h"
#include <SD.h>

static uint8_t sdHost() {
#if defined(SPI2_HOST)
  return SPI2_HOST;
#elif defined(HSPI)
  return HSPI;
#else
  return 2;
#endif
}

SdManager::SdManager() : _spi(sdHost()), _mounted(false) {}

bool SdManager::isMounted() const { return _mounted; }

bool SdManager::mount(uint32_t freqHz) {
  if (_mounted) return true;

  // Dedicated SPI host for SD (do NOT use FSPI on ESP32-S3; that is for flash/psram)
  _spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  bool ok = SD.begin(PIN_SD_CS, _spi, freqHz);
  _mounted = ok;
  return ok;
}

bool SdManager::mountWithTimeout(uint32_t timeoutMs, uint32_t freqHz) {
  (void)timeoutMs; // API kept; no blocking sleeps
  return mount(freqHz);
}
