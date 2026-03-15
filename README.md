# TempNode

> Ethernet-first DS18B20 telemetry node for **Waveshare ESP32-S3-ETH** (ESP32-S3 + W5500).

![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange?logo=platformio)
![Framework](https://img.shields.io/badge/Framework-Arduino-blue)
![Network](https://img.shields.io/badge/Network-W5500%20Ethernet-success)
![OTA](https://img.shields.io/badge/OTA-Token%20Gated-critical)
[![Version](https://img.shields.io/github/v/release/dkennerknecht/TempNode?display_name=tag&sort=semver)](https://github.com/dkennerknecht/TempNode/releases/latest)
[![Release](https://img.shields.io/github/release-date/dkennerknecht/TempNode)](https://github.com/dkennerknecht/TempNode/releases/latest)
[![Docs](https://img.shields.io/badge/docs-ReDoc%20%2B%20AsyncAPI-0A7EA4)](https://dkennerknecht.github.io/TempNode/)
[![API Docs Workflow](https://github.com/dkennerknecht/TempNode/actions/workflows/api-docs-pages.yml/badge.svg)](https://github.com/dkennerknecht/TempNode/actions/workflows/api-docs-pages.yml)
[![Release Workflow](https://github.com/dkennerknecht/TempNode/actions/workflows/release-firmware.yml/badge.svg)](https://github.com/dkennerknecht/TempNode/actions/workflows/release-firmware.yml)

TempNode reads DS18B20 sensors, exposes a REST API, publishes MQTT telemetry, writes history/logs to SD, and supports guarded firmware and LittleFS OTA updates.

## Highlights

- Ethernet-first runtime for stable, low-latency telemetry.
- REST API with OpenAPI contract and generated ReDoc site.
- MQTT topics for JSON and float-only payload integration.
- Config migration + runtime-safe config patch endpoints.
- SD-backed history, metrics, logging, and offline MQTT queue persistence.
- OTA with auth, hash validation, and downgrade protection.

## Contents

- [Documentation](#documentation)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [REST API](#rest-api)
- [MQTT API](#mqtt-api)
- [OTA Versioning and Releases](#ota-versioning-and-releases)
- [Hardware](#hardware)
- [Development](#development)
- [Repository Layout](#repository-layout)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

## Documentation

Published docs (GitHub Pages):

- REST API (ReDoc): [dkennerknecht.github.io/TempNode](https://dkennerknecht.github.io/TempNode/)
- MQTT API (AsyncAPI HTML): [dkennerknecht.github.io/TempNode/mqtt](https://dkennerknecht.github.io/TempNode/mqtt/)

Published specs:

- OpenAPI JSON: [openapi.json](https://dkennerknecht.github.io/TempNode/openapi.json)
- AsyncAPI YAML: [asyncapi.yaml](https://dkennerknecht.github.io/TempNode/asyncapi.yaml)

Repository docs:

- [`docs/openapi.json`](docs/openapi.json)
- [`docs/asyncapi.yaml`](docs/asyncapi.yaml)
- [`VERSIONING.md`](VERSIONING.md)
- [`SMOKETEST.md`](SMOKETEST.md)

## Quick Start

### 1. Prerequisites

- PlatformIO Core (`pio`)
- USB connection to board
- Optional SD card (FAT32)
- Optional MQTT broker

### 2. Configure Device

```bash
cp config.example.json data/config.json
```

### 3. Build and Flash

```bash
pio run -e esp32s3 -t upload -t uploadfs -t monitor
```

This uploads firmware + LittleFS (`data/`), then opens the serial monitor.

### 4. Verify

```bash
NODE_IP=<your-node-ip>
curl -sS "http://$NODE_IP/api/v1/health"
curl -sS "http://$NODE_IP/api/v1/system"
```

## Configuration

Runtime config path is `/config.json`.

Load order:

1. LittleFS `/config.json` (from `data/config.json` + `uploadfs`)
2. SD `/config.json` (optional override)

Schema and migration:

- `configVersion` (`2` = current)
- older configs are auto-migrated and written back

Main config blocks:

- `network` (DHCP/static IP, hostname)
- `sensors` (interval, DS18B20 resolution/timeout)
- `rest` (enable/port)
- `mqtt` (host/port/auth/topics/reconnect/offline queue/health publish)
- `history` (path/flush/retention)
- `logging` (separate console + SD levels, rotation, retention)
- `metrics`, `watchdog`, `security`, `ota`

Reference:

- [`config.example.json`](config.example.json)

## REST API

Base URL:

- `http://<NODE_IP>/api/v1`

Auth behavior (via `security.restAuthMode`):

- `anonymous`: no auth required
- `token`: Bearer token required
- optional in token mode: `security.allowAnonymousGet=true` for unauthenticated `GET`

Key endpoints:

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Detailed subsystem health (`network`, `mqtt`, `sd`, `time`, `ota_state`) |
| GET | `/temps` | Latest reading per discovered sensor |
| GET | `/temp?sensorId=<id>` | Full JSON for one sensor |
| GET | `/temp/float?sensorId=<id>` | Float-only response (`text/plain`) |
| GET/POST/PUT | `/sensors/interval` | Read/set polling interval with persistence |
| GET | `/system` | Runtime and build diagnostics |
| GET | `/metrics` | Runtime + persisted counters |
| GET | `/history` | History tail (`sensor`, `limit`) |
| GET/PUT | `/config` | Secure full config read/patch |
| GET/PUT | `/config/mqtt` | Secure MQTT-only config read/patch |
| POST | `/ota` | Firmware upload |
| POST | `/ota/fs` | LittleFS upload |

Example requests:

```bash
# float-only value for Loxone-like integrations
curl -sS "http://$NODE_IP/api/v1/temp/float?sensorId=28701749000000D8"

# set interval via JSON body
curl -sS -X PUT \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"intervalMs":20000}' \
  "http://$NODE_IP/api/v1/sensors/interval"

# query fallback
curl -sS -X PUT \
  -H "Authorization: Bearer $TOKEN" \
  "http://$NODE_IP/api/v1/sensors/interval?intervalMs=20000"
```

## MQTT API

Default topic base: `device`.

| Topic | Payload |
|---|---|
| `device/<deviceId>/temps/<sensorId>` | JSON sensor reading |
| `device/<deviceId>/temps_float/<sensorId>` | float-only payload |
| `device/<deviceId>/system` | runtime snapshot |
| `device/<deviceId>/health` | health metrics |
| `device/<deviceId>/status` | LWT retained (`online`/`offline`) |

Behavior notes:

- MQTT auth uses `mqtt.user` + `mqtt.pass`; if both empty, anonymous login is used.
- `deviceId` is sanitized (invalid chars replaced with `_`).
- default short `deviceId` format if empty: `esp32s3_003CFD`.
- optional SD-backed offline queue:
  - `mqtt.offlinePersistSdEnabled`
  - `mqtt.offlinePersistPath`
  - `mqtt.offlinePersistMaxLines`
- health publish controls:
  - `mqtt.publishHealth`
  - `mqtt.healthIntervalMs`

## OTA Versioning and Releases

OTA endpoint activation requirements:

- `ota.enabled=true`
- `ota.allowInsecureHttp=true`
- `security.restAuthMode="token"`
- `security.restToken` set

OTA validation includes:

- Bearer token
- image name `.bin`
- `Content-Length`
- partition-size fit
- hash headers (`X-OTA-SHA256` recommended, `X-OTA-MD5` supported)
- version gate (`ota.allowDowngrade=false`)

Version model:

- local build version: `X.Y.Z.N` (internal 4th segment for monotonic OTA safety)
- release tag version: `vX.Y.Z`
- release base configured in `platformio.ini` via `custom_release_version`
- version details: [`VERSIONING.md`](VERSIONING.md)

GitHub Releases:

- workflow: [`.github/workflows/release-firmware.yml`](.github/workflows/release-firmware.yml)
- trigger: push tag `vX.Y.Z`
- assets:
  - `TempNode-<version>-firmware.bin`
  - `TempNode-<version>-littlefs.bin`
  - `SHA256SUMS.txt`
  - `openapi.json`
  - `asyncapi.yaml`

Release example:

```bash
git tag -a v1.2.2 -m "Release v1.2.2"
git push origin v1.2.2
```

## Hardware

### Target Board

- Waveshare ESP32-S3-ETH
- W5500 Ethernet via SPI
- TF/SD slot via SPI
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

### Board Reference

![Waveshare ESP32-S3-ETH board](ESP32-S3-ETH.jpg)

## Development

Common commands:

```bash
# build
pio run -e esp32s3

# flash firmware + littlefs
pio run -e esp32s3 -t upload -t uploadfs

# monitor
pio device monitor

# native unit tests
pio test -e native

# OpenAPI route contract check
python3 scripts/contract_test_openapi.py

# build docs locally (ReDoc + AsyncAPI HTML)
./scripts/build_api_docs.sh
```

Quality gates used in repo:

- OpenAPI route contract test (`scripts/contract_test_openapi.py`)
- GitHub Pages doc generation (`api-docs-pages.yml`)
- Tag-based release build + assets (`release-firmware.yml`)

## Repository Layout

- [`src/main.cpp`](src/main.cpp): boot sequence + runtime wiring
- [`src/RestServer.cpp`](src/RestServer.cpp): REST routes + OTA handling
- [`src/MqttClientManager.cpp`](src/MqttClientManager.cpp): MQTT lifecycle + publishing
- [`src/ConfigManager.cpp`](src/ConfigManager.cpp): load/validate/migrate/persist config
- [`src/HistoryManager.cpp`](src/HistoryManager.cpp): history flush + retention
- [`docs/openapi.json`](docs/openapi.json): REST contract
- [`docs/asyncapi.yaml`](docs/asyncapi.yaml): MQTT contract

## Troubleshooting

### MQTT connects to wrong host

Most common reason is stale runtime config in LittleFS/SD.

```bash
# update data/config.json, then re-upload filesystem
pio run -e esp32s3 -t uploadfs
```

Verify with `GET /api/v1/health` at `checks.mqtt.host`.

### `/api/v1/sensors/interval` says `missing intervalMs`

Use one of these supported forms:

- query/form: `intervalMs=...`
- JSON body: `{"intervalMs":20000}` with `Content-Type: application/json`

### `/health` is `degraded`

Inspect `checks.network`, `checks.mqtt`, `checks.sd`, `checks.time`, `checks.ota_state` to identify the failing subsystem.

### Build issues with dependencies

```bash
pio run -t clean
pio run -e esp32s3
```

## Contributing

Issues and pull requests are welcome.

For hardware validation before merge, use:

- [`SMOKETEST.md`](SMOKETEST.md)

## License

This project is licensed under **GNU General Public License v3.0**.

- Full text: [`LICENSE`](LICENSE)
