#include "SharedSdMutex.h"

SemaphoreHandle_t sharedSdMutex() {
  static SemaphoreHandle_t mutex = nullptr;
  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
  }
  return mutex;
}
