# ESP32-S3-ETH (Waveshare) – DS18B20 + REST + MQTT + SD Logging

Produktionsnahes PlatformIO-Projekt (Arduino Framework) für das **Waveshare ESP32-S3-ETH**:
- W5500 Ethernet (SPI)
- Onboard TF/SD (SPI)
- DS18B20 (1-Wire)

## Features
- **Event-driven**, keine `delay()` im Betrieb (nur `delay(0)` für Yield)
- DS18B20 **Auto-Discovery** (ROM-ID als Sensor-ID)
- **Nicht-blockierende Messung** (`setWaitForConversion(false)` + State Machine)
- **Logging**: Serial immer, SD optional
  - Format: `YYYY-MM-DD HH:MM:SS,mmm;LEVEL;Message`
  - Daily Rotation
  - Queue + Worker Task, Drop-Oldest bei Overflow
- **History**: JSONL auf SD (append-only), REST Tail-Query
- **REST API** (ESPAsyncWebServer)
- **MQTT** (AsyncMqttClient) inkl. LWT + Offline-Ringbuffer pro Sensor
- **Watchdog (optional)** via `esp_task_wdt`

## REST Endpoints
- `GET /api/v1/temps`
- `GET /api/v1/system`
- `GET /api/v1/history?sensor=<id>&limit=100`
- `GET /api/v1/health`
- `GET /api/v1/metrics`
- Optional: `POST /api/v1/ota` (wenn `ota.enabled=true`)
  - OTA wird nur aktiviert, wenn zusätzlich `security.enabled=true`, `security.restToken` gesetzt und `ota.allowInsecureHttp=true` ist.

## MQTT Topics
- `device/<deviceId>/temps/<sensorId>` (retained)
- `device/<deviceId>/system` (retained)
- `device/<deviceId>/status` (LWT retained)

## Config
- Defaults im Code
- Optional Override via SD: `/config.json`
- Bei `security.enabled=true` muss REST-Auth konfiguriert sein (`restToken` via `Authorization: Bearer ...` oder `restUser`/`restPass`)
- Query-Parameter `?token=` wird aus Sicherheitsgründen nicht unterstützt
- Für OTA gilt: Bearer-Token Pflicht, Versions-Downgrade standardmäßig blockiert (`ota.allowDowngrade=false`)

Siehe `config.example.json`.

### Fail-fast
Wenn `/config.json` **vorhanden** ist, aber nicht lesbar/parst, stoppt das System absichtlich.

## Build
```bash
pio run
pio run -t upload
pio device monitor
```

## Smoke Test
Siehe `SMOKETEST.md` für eine komplette Hardware-Checkliste (Boot, REST, MQTT, SD, Security, Persistenz).

## Notes
- Ethernet basiert auf `ETH.begin(ETH_PHY_W5500, ...)` (Arduino-ESP32 Core 3.x).
- Wenn du eine andere Core-Version nutzt und die Signatur abweicht, passe `NetworkManager.cpp` an.
