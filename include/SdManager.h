#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <driver/spi_master.h>

class SdManager {
public:
  SdManager();

  bool mount(uint32_t freqHz = 20000000);
  bool mountWithTimeout(uint32_t timeoutMs, uint32_t freqHz = 20000000);

  bool isMounted() const;

private:
  SPIClass _spi;
  bool _mounted;
};
