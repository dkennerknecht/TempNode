#include "OtaHealthManager.h"
#include "Config.h"
#include "LogManager.h"
#include "AppNetworkManager.h"
#include <esp_ota_ops.h>

#if defined(CONFIG_APP_ROLLBACK_ENABLE)
extern "C" bool verifyRollbackLater() {
  // Defer image confirmation to the application-level health gate.
  return true;
}
#endif

void OtaHealthManager::begin(const OtaConfig& cfg, LogManager& log, AppNetworkManager& net) {
  _cfg = &cfg;
  _log = &log;
  _net = &net;
  _pendingVerify = false;
  _bootMs = millis();

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;

  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    _pendingVerify = true;
    _log->warn("OTA image pending verification");
  }
}

void OtaHealthManager::loop() {
  if (!_cfg || !_log || !_net) return;
  if (!_pendingVerify) return;

  uint32_t elapsed = (uint32_t)(millis() - _bootMs);
  if (elapsed < _cfg->healthConfirmMs) return;

  if (_cfg->requireNetworkForConfirm && !_net->isUp()) {
    // Give networking additional time; then fail-safe to rollback.
    if (elapsed < (_cfg->healthConfirmMs * 4U)) return;

    _log->error("OTA verification timeout (network still down), rolling back");
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    _log->info("OTA image confirmed valid");
  } else {
    _log->error(String("OTA confirm failed err=") + (int)err);
  }
  _pendingVerify = false;
}
