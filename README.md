<div align="center">
  <h1>TempNode</h1>

  <p><strong>Ethernet-first DS18B20 telemetry node for Waveshare ESP32-S3-ETH</strong></p>
  <p>REST API | MQTT | SD Logging | Watchdog | Secure OTA</p>

  ![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange?logo=platformio)
  ![Framework](https://img.shields.io/badge/Framework-Arduino-blue)
  ![Network](https://img.shields.io/badge/Network-W5500%20Ethernet-success)
  ![OTA](https://img.shields.io/badge/OTA-Token%20Protected-critical)
</div>

TempNode is a production-oriented PlatformIO project (Arduino framework) for an Ethernet-connected temperature node.
It reads DS18B20 sensors, exposes data over REST, publishes to MQTT, stores logs/history on SD, and supports secured OTA firmware updates.

## Table of Contents

- [Overview](#overview)
- [Tech Stack](#tech-stack)
- [Hardware and Pinout](#hardware-and-pinout)
- [Quick Start](#quick-start)
- [Configuration (`/config.json`)](#configuration-configjson)
- [REST API](#rest-api)
- [API Docs (ReDoc + AsyncAPI + GitHub Pages)](#api-docs-redoc--asyncapi--github-pages)
- [MQTT](#mqtt)
- [OTA Update](#ota-update)
- [SD Persistence](#sd-persistence)
- [Smoke Test](#smoke-test)
- [Troubleshooting](#troubleshooting)
- [Development](#development)
- [API Contract](#api-contract)
- [License](#license)

## Overview

### Core Features

- Event-driven runtime without blocking `delay()` loops (`delay(0)` only for yield).
- DS18B20 auto-discovery (sensor ID = ROM ID in hex).
- Non-blocking measurement state machine (`request -> wait -> read`).
- REST API on configurable port.
- MQTT publishing with retained topics, LWT (`online/offline`), and per-sensor offline ring buffers.
- Asynchronous logging (Serial always, SD optional) with queue + worker task.
- Temperature history stored as JSONL on SD with REST tail query.
- Persistent runtime statistics (`/stats.json`) using safe write (`.tmp` + rename).
- Optional task watchdog support.
- Optional OTA updates with authentication, size checks, and version gating.

### Runtime Behavior

- Staged startup sequence (network -> sensors -> MQTT) for more robust boot behavior.
- Time source starts as uptime and switches to NTP when network is available.
- Deliberate fail-fast halt if `/config.json` exists but cannot be read or parsed.

## Tech Stack

- PlatformIO
- Arduino framework for ESP32-S3
- Board target: `esp32-s3-devkitc-1`
- Platform: `https://github.com/pioarduino/platform-espressif32.git`
- Key libraries:
  - `ESPAsyncWebServer`
  - `AsyncTCP`
  - `AsyncMqttClient`
  - `DallasTemperature`
  - `OneWire`
  - `ArduinoJson`

Note: A pre-build script (`scripts/patch_onewire.py`) patches the locally installed OneWire source to silence a known warning pattern.

## Hardware and Pinout

![Waveshare ESP32-S3-ETH board](ESP32-S3-ETH.jpg)

### Target Board

- Waveshare ESP32-S3-ETH
- W5500 Ethernet over SPI
- Onboard TF/SD slot over SPI
- DS18B20 on 1-Wire

### Pin Mapping

| Function | Pin |
|---|---|
| ETH MISO | GPIO12 |
| ETH MOSI | GPIO11 |
| ETH SCLK | GPIO13 |
| ETH CS | GPIO14 |
| ETH RST | GPIO9 |
| ETH INT | GPIO10 |
| SD CS | GPIO4 |
| SD MOSI | GPIO6 |
| SD MISO | GPIO5 |
| SD SCK | GPIO7 |
| 1-Wire Data (DS18B20) | GPIO17 |
| RGB LED off-at-boot pins | GPIO21 / GPIO38 |

## Quick Start

### 1. Prerequisites

- PlatformIO Core installed (`pio` CLI available)
- USB connection to the board
- Optional SD card (FAT32)
- Optional MQTT broker

### 2. Build

```bash
pio run
```

### 3. Flash

```bash
pio run -t upload -t uploadfs
```

### 4. Serial Monitor

```bash
pio device monitor
```

### 5. Configuration File

- Project config for `uploadfs` lives at [`data/config.json`](data/config.json).
- Use [`config.example.json`](config.example.json) as template.
- Optional runtime override: place `/config.json` on SD card root.

## Configuration (`/config.json`)

- A valid `/config.json` is required (fail-fast if missing).
- Load order:
  1. LittleFS `/config.json` (from `pio run -t uploadfs`)
  2. SD `/config.json` (if present, overrides LittleFS values)
- If a config exists but is unreadable or invalid JSON, the firmware intentionally halts (fail-fast).

### Main Sections

- `network`: DHCP or static IP (`ip/gw/mask/dns`)
- `sensors`: interval, resolution, conversion timeout
- `rest`: enable/disable and port
- `mqtt`: broker, optional auth (`user/pass`), topic base, reconnect bounds, offline buffer size, health publish toggle/interval
- `history`: JSONL path + flush behavior
- `logging`: per-target log levels (console/SD) plus SD rotation/retention
- `metrics`: toggle `/api/v1/metrics`
- `watchdog`: optional task WDT
- `security`: REST auth settings
- `ota`: OTA endpoint gating, downgrade policy, health confirmation window

### Security Rules

When `security.enabled=true`, at least one REST auth method must be configured:

- Bearer token (`security.restToken`)
- or Basic auth (`security.restUser` + `security.restPass`)

Query token auth (`?token=`) is intentionally not supported.

## REST API

Base URL: `http://<NODE_IP>/api/v1`

If `security.enabled=true`, endpoints require authentication.

### Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/health` | Returns `status` plus detailed checks (`network`, `mqtt`, `sd`, `time`, `ota_state`) |
| GET | `/temps` | Latest readings for all discovered sensors |
| GET | `/sensors` | Sensor summary plus current `intervalMs` |
| GET | `/sensors/interval` | Read current sensor interval |
| POST | `/sensors/interval` | Set interval via query, form, or JSON body |
| PUT | `/sensors/interval` | Same as POST; semantic update endpoint |
| GET | `/system` | System diagnostics (IP, heap, uptime, chip, boot stats) |
| GET | `/metrics` | Runtime metrics + persistent stats |
| GET | `/history?sensor=<id>&limit=<n>` | Tail history entries as JSON array (`limit` 1..1000) |
| POST | `/ota` | Firmware upload endpoint (conditionally enabled) |

### Example Calls

No auth (only if security is disabled):

```bash
curl -sS "http://$NODE_IP/api/v1/health"
curl -sS "http://$NODE_IP/api/v1/temps"
```

`/health` response now includes per-subsystem detail fields under `checks.*` (for example `checks.network.up`, `checks.mqtt.connected`, `checks.mqtt.resolveOk`, `checks.mqtt.tcpProbeOpen`, `checks.sd.available`, `checks.sd.diskUsedBytes`, `checks.sd.usagePercent`, `checks.time.valid`, `checks.ota_state.imageState`).

Bearer token:

```bash
TOKEN="<your-token>"
curl -sS -H "Authorization: Bearer $TOKEN" "http://$NODE_IP/api/v1/system"
```

Basic auth:

```bash
curl -u api:yourpassword "http://$NODE_IP/api/v1/system"
```

## API Docs (ReDoc + AsyncAPI + GitHub Pages)

This repository includes automated REST and MQTT docs build + GitHub Pages deployment:

- Workflow: [`.github/workflows/api-docs-pages.yml`](.github/workflows/api-docs-pages.yml)
- REST spec: [`docs/openapi.json`](docs/openapi.json)
- MQTT spec: [`docs/asyncapi.yaml`](docs/asyncapi.yaml)
- Contract check: [`scripts/contract_test_openapi.py`](scripts/contract_test_openapi.py)

### Hosted docs URL

After enabling GitHub Pages for this repository (source: **GitHub Actions**), docs are published at:

- REST docs (ReDoc): `https://dkennerknecht.github.io/TempNode/`
- MQTT docs (AsyncAPI HTML): `https://dkennerknecht.github.io/TempNode/mqtt/`

Published raw specs:

- OpenAPI: `https://dkennerknecht.github.io/TempNode/openapi.json`
- AsyncAPI: `https://dkennerknecht.github.io/TempNode/asyncapi.yaml`

### Local generation

```bash
./scripts/build_api_docs.sh
```

This runs:

- OpenAPI contract test
- Redocly lint
- AsyncAPI validation
- ReDoc static HTML build to `site/index.html`
- AsyncAPI MQTT HTML build to `site/mqtt/index.html`

## MQTT

Broker authentication is configured via `mqtt.user` and `mqtt.pass`.
If both are empty, the client attempts anonymous MQTT login.
Legacy fallback from `security.mqttUser`/`security.mqttPass` is still supported for older configs.
On reconnect attempts, serial logs include host, port, user, clientId, plus diagnostics (DNS resolve, ping, TCP probe on configured and common alternate MQTT port).
Health payload publishing can be configured via `mqtt.publishHealth` and `mqtt.healthIntervalMs`.

### Topic Layout

Default (`baseTopic = device`):

- `device/<deviceId>/temps/<sensorId>` (retained)
- `device/<deviceId>/system` (retained)
- `device/<deviceId>/health` (retained)
- `device/<deviceId>/status` (retained, LWT)

### Payload Examples

`temps/<sensorId>`:

```json
{
  "timestamp": 1741862400123,
  "value": 23.56,
  "unit": "C",
  "sensorId": "28FF641D2A1603A1",
  "status": 0,
  "timeSource": 0,
  "timeValid": true
}
```

`status`:

```json
{
  "timestamp": 1741862400123,
  "status": "online",
  "ip": "192.168.1.50",
  "link": true
}
```

Note: In the current build, MQTT TLS is not supported by the selected client stack. If `mqtt.tls=true`, MQTT is disabled (fail-closed, no plaintext fallback).

## OTA Update

### Requirements

In `/config.json`:

- `ota.enabled = true`
- `ota.allowInsecureHttp = true` (explicit opt-in because REST server is plain HTTP)
- `security.enabled = true`
- `security.restToken` configured
- `ota.requireHashHeader = true` (default; requires upload hash header)

### Validation and Safety Rules

- Requires `Authorization: Bearer <token>`.
- Firmware filename must end in `.bin`.
- `Content-Length` must be present.
- Firmware size is checked against OTA target partition.
- Integrity header is required by default: `X-OTA-SHA256` (recommended) or `X-OTA-MD5` (legacy).
- By default, downgrade or same-version uploads are rejected (`ota.allowDowngrade=false`).

### Upload Example

```bash
NODE_IP=192.168.1.50
TOKEN="<your-token>"
SHA256="$(shasum -a 256 .pio/build/esp32s3/firmware.bin | awk '{print $1}')"

curl -i \
  -H "Authorization: Bearer $TOKEN" \
  -H "X-OTA-SHA256: $SHA256" \
  -F "firmware=@.pio/build/esp32s3/firmware.bin" \
  "http://$NODE_IP/api/v1/ota"
```

Success behavior:

- `HTTP 200` with `{"status":"ok","reboot":true}`
- device reboots automatically

### OTA Health Confirmation

After an OTA boot, image validation is deferred to a health gate:

- Waits `ota.healthConfirmMs`
- Optional network dependency: `ota.requireNetworkForConfirm`
- If the network does not recover within the extended window, rollback is requested

## SD Persistence

Typical files:

- `/config.json` (optional, user-provided)
- `/history.jsonl` (temperature history)
- `/stats.json` (persistent counters)
- `/log-YYYYMMDD.log` or `/log-uptime.log` (runtime logs)

### Log Rotation and Retention

Configured in `/config.json` under `logging`:

- `logging.consoleLevel`: `DEBUG|INFO|WARN|ERROR`
- `logging.sdLevel`: `DEBUG|INFO|WARN|ERROR`
- `logging.sdEnabled`: enable/disable SD log writes
- `logging.rotateDaily`: rotate daily (`/log-YYYYMMDD.log`) or single file (`/log.log`)
- `logging.retentionDays`: delete old rotated log files (only when `rotateDaily=true`, `0` disables retention)

### Log Format

```text
YYYY-MM-DD HH:MM:SS,mmm;LEVEL;Message
```

If NTP is not available, uptime-based timestamps are used.

## Smoke Test

Full hardware validation checklist is available in [`SMOKETEST.md`](SMOKETEST.md).

## Troubleshooting

### `pio run` fails because of dependencies

```bash
pio run -t clean
pio run
```

Also ensure internet access is available for resolving `lib_deps`.

### REST API not reachable

- Check serial logs for `ETH got IP`.
- Use the IP shown in logs.
- If security is enabled, send valid auth headers.

### `/api/v1/health` returns `degraded`

- Expected when `mqtt.enabled=true` but broker is disconnected.
- Verify broker reachability and `mqtt.*` config.

### OTA endpoint not available

Check all OTA prerequisites:

- `ota.enabled`
- `ota.allowInsecureHttp`
- `security.enabled`
- `security.restToken`

### No sensor data

- Verify DS18B20 wiring and pull-up resistor.
- Confirm 1-Wire line is connected to GPIO17.
- If discovery fails, logs report `no DS18B20 found`.

## Development

### Key Files

- [`src/main.cpp`](src/main.cpp): boot sequence, runtime loop, module wiring
- [`src/RestServer.cpp`](src/RestServer.cpp): REST endpoints and OTA upload handling
- [`src/MqttClientManager.cpp`](src/MqttClientManager.cpp): MQTT connection, buffering, publishing
- [`src/SensorManager.cpp`](src/SensorManager.cpp): DS18B20 measurement state machine
- [`src/OtaHealthManager.cpp`](src/OtaHealthManager.cpp): OTA health-confirm and rollback logic
- [`config.example.json`](config.example.json): config template
- [`SMOKETEST.md`](SMOKETEST.md): hardware smoke-test checklist

### Useful Commands

```bash
pio run
pio run -t upload -t uploadfs
pio device monitor
```

## API Contract

- OpenAPI spec: [`docs/openapi.json`](docs/openapi.json)
- AsyncAPI spec: [`docs/asyncapi.yaml`](docs/asyncapi.yaml)
- Contract test (spec vs. registered REST routes):

```bash
python3 scripts/contract_test_openapi.py
npx --yes @asyncapi/cli@latest validate docs/asyncapi.yaml
```

## License

No license file is currently included. For a public repository, add a suitable `LICENSE`.
