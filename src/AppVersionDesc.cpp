#include <esp_app_desc.h>
#include <esp_idf_version.h>

#ifndef TEMPNODE_APP_VERSION
#define TEMPNODE_APP_VERSION "0.0.0-dev"
#endif

#ifndef IDF_VER
#define IDF_VER "unknown"
#endif

extern "C" {

// Strong override for framework weak default descriptor.
// This ensures OTA version checks use a project-specific build version.
extern const esp_app_desc_t esp_app_desc __attribute__((used, section(".rodata_desc"))) = {
  ESP_APP_DESC_MAGIC_WORD,
  0,
  {0, 0},
  TEMPNODE_APP_VERSION,
  "TempNode",
  __TIME__,
  __DATE__,
  IDF_VER,
  {0},
  0,
  0,
  0,
  {0, 0, 0},
  {0},
};

} // extern "C"
