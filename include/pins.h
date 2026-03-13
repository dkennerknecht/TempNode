#pragma once
#include <Arduino.h>

// ========================
// Waveshare ESP32-S3-ETH (fixed pinout)
// ========================

// W5500 Ethernet (SPI)
static constexpr gpio_num_t PIN_ETH_MISO = GPIO_NUM_12;
static constexpr gpio_num_t PIN_ETH_MOSI = GPIO_NUM_11;
static constexpr gpio_num_t PIN_ETH_SCLK = GPIO_NUM_13;
static constexpr gpio_num_t PIN_ETH_CS   = GPIO_NUM_14;
static constexpr gpio_num_t PIN_ETH_RST  = GPIO_NUM_9;
static constexpr gpio_num_t PIN_ETH_INT  = GPIO_NUM_10;

// TF/SD Slot (SPI)
static constexpr gpio_num_t PIN_SD_CS   = GPIO_NUM_4;
static constexpr gpio_num_t PIN_SD_MOSI = GPIO_NUM_6;
static constexpr gpio_num_t PIN_SD_MISO = GPIO_NUM_5;
static constexpr gpio_num_t PIN_SD_SCK  = GPIO_NUM_7;

// DS18B20 (1-Wire)
static constexpr gpio_num_t PIN_1W_DATA = GPIO_NUM_17;

// Onboard RGB LED (WS2812). On many ESP32-S3-ETH boards it's GPIO21;
// some variants route it differently. We drive both pins "off" at boot.
static constexpr gpio_num_t PIN_RGB_DATA_PRIMARY = GPIO_NUM_21;
static constexpr gpio_num_t PIN_RGB_DATA_ALT     = GPIO_NUM_38;
