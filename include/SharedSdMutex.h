#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Shared lock for all SD card accesses across tasks/components.
SemaphoreHandle_t sharedSdMutex();
