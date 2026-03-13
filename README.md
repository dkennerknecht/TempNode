# TempNode

Produktionsnahes PlatformIO-Projekt (Arduino Framework) fuer einen Ethernet-basierten Temperatur-Node auf dem **Waveshare ESP32-S3-ETH**.

TempNode liest DS18B20-Sensoren zyklisch aus, stellt Werte per REST bereit, publiziert per MQTT, schreibt Logs/History auf SD und unterstuetzt abgesicherte OTA-Updates.

![Waveshare ESP32-S3-ETH](ESP32-S3-ETH.jpg)

## Inhalt

- [Ueberblick](#ueberblick)
- [Technik-Stack](#technik-stack)
- [Hardware und Pinout](#hardware-und-pinout)
- [Schnellstart](#schnellstart)
- [Konfiguration (`/config.json`)](#konfiguration-configjson)
- [REST API](#rest-api)
- [MQTT](#mqtt)
- [OTA Update](#ota-update)
- [Persistenz auf SD](#persistenz-auf-sd)
- [Smoke Test](#smoke-test)
- [Troubleshooting](#troubleshooting)
- [Entwicklung](#entwicklung)

## Ueberblick

### Kernfunktionen

- Event-driven Laufzeit ohne blockierende `delay()`-Schleifen (nur `delay(0)` fuer Yield).
- DS18B20 Auto-Discovery (Sensor-ID = ROM-ID als Hex-String).
- Nicht-blockierende Messung via State Machine (`request -> wait -> read`).
- REST API auf konfigurierbarem Port.
- MQTT Publishing inkl. LWT (`online/offline`) und Offline-Ringbuffer pro Sensor.
- Asynchrones Logging (Serial immer, SD optional) mit Queue + Worker Task.
- Temperatur-History als JSONL auf SD mit REST-Tail-Abfrage.
- Persistente Betriebsstatistiken (`/stats.json`) mit sicheren Writes (`.tmp` + rename).
- Optionale Watchdog-Integration.
- Optionale OTA-Updates mit Sicherheits- und Versions-Pruefung.

### Laufzeitverhalten

- Gestaffelter Startup (Netzwerk -> Sensoren -> MQTT), um Boot robust zu halten.
- Zeitbasis startet mit Uptime und wechselt auf NTP, sobald Netzwerk verfuegbar ist.
- Bei vorhandenem, aber unlesbarem `/config.json` greift bewusst ein **Fail-Fast Halt**.

## Technik-Stack

- PlatformIO
- Arduino Framework fuer ESP32-S3
- Board: `esp32-s3-devkitc-1`
- Plattform: `https://github.com/pioarduino/platform-espressif32.git`
- Kernbibliotheken:
  - `ESPAsyncWebServer`
  - `AsyncTCP`
  - `AsyncMqttClient`
  - `DallasTemperature`
  - `OneWire`
  - `ArduinoJson`

Hinweis: Ein Pre-Build-Script (`scripts/patch_onewire.py`) patched lokal die installierte OneWire-Version, um bekannte Warnungen zu vermeiden.

## Hardware und Pinout

### Zielboard

- Waveshare ESP32-S3-ETH
- Ethernet PHY: W5500 (SPI)
- Onboard TF/SD Slot (SPI)
- DS18B20 an 1-Wire

### Pinbelegung

| Funktion | Pin |
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
| RGB LED (off at boot, primary/alt) | GPIO21 / GPIO38 |

## Schnellstart

### 1. Voraussetzungen

- PlatformIO Core installiert (`pio` CLI verfuegbar)
- USB-Zugang zum Board
- Optional: SD-Karte (FAT32)
- Optional: MQTT Broker

### 2. Build

```bash
pio run
```

### 3. Flashen

```bash
pio run -t upload
```

### 4. Monitor

```bash
pio device monitor
```

### 5. Optional: Konfiguration auf SD

- Datei [`config.example.json`](config.example.json) als Vorlage nutzen.
- Als `/config.json` im Root der SD-Karte ablegen.

## Konfiguration (`/config.json`)

- Defaults sind im Code hinterlegt.
- `/config.json` ist optional.
- Falls Datei existiert und nicht lesbar/parst: absichtlicher Halt (Fail-Fast).

### Wichtige Bereiche

- `network`: DHCP oder statische IP (`ip/gw/mask/dns`)
- `sensors`: Intervall, Aufloesung, Conversion-Timeout
- `rest`: Endpoint aktiv + Port
- `mqtt`: Broker, Topic-Basis, Reconnect-Bounds, Buffer-Groesse
- `history`: JSONL-Pfad + Flush
- `metrics`: `/api/v1/metrics` aktiv/deaktivierbar
- `watchdog`: optionaler Task-WDT
- `security`: REST/MQTT Auth
- `ota`: OTA-Gating, Downgrade-Regel, Health-Confirm

### Security-Regeln

Wenn `security.enabled=true`, muss mindestens eine REST-Auth-Methode gesetzt sein:

- Bearer-Token (`security.restToken`)
- oder Basic Auth (`security.restUser` + `security.restPass`)

`?token=` als Query-Parameter wird absichtlich **nicht** akzeptiert.

## REST API

Basis: `http://<NODE_IP>/api/v1`

Wenn `security.enabled=true`, sind Endpoints authentifiziert.

### Endpoints

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/health` | `ok` oder `degraded` (degraded z. B. bei aktivem, aber getrenntem MQTT) |
| GET | `/temps` | Liste aller letzten Sensorwerte |
| GET | `/sensors` | Zusammenfassung inkl. `intervalMs` und letzter Werte |
| GET | `/sensors/interval` | Aktuelles Sensor-Intervall |
| GET | `/sensors/interval?intervalMs=...` | Setzt Intervall und liefert Rueckgabe |
| POST | `/sensors/interval` | Setzt Intervall (`query`, `form` oder JSON Body) |
| GET | `/system` | Systeminfos (IP, Heap, Uptime, Chip, BootCount, ResetReason) |
| GET | `/metrics` | Laufzeitmetriken + persistente Stats |
| GET | `/history?sensor=<id>&limit=<n>` | Letzte History-Eintraege (JSON Array), `limit` 1..1000 |
| POST | `/ota` | Firmware Upload (nur wenn OTA aktiv und freigeschaltet) |

### Beispiele

Ohne Auth (nur wenn Security aus):

```bash
curl -sS "http://$NODE_IP/api/v1/health"
curl -sS "http://$NODE_IP/api/v1/temps"
```

Mit Bearer-Token:

```bash
TOKEN="<dein-token>"
curl -sS -H "Authorization: Bearer $TOKEN" "http://$NODE_IP/api/v1/system"
```

Mit Basic Auth:

```bash
curl -u api:meinpass "http://$NODE_IP/api/v1/system"
```

## MQTT

### Topic-Schema

Standard (`baseTopic = device`):

- `device/<deviceId>/temps/<sensorId>` (retained)
- `device/<deviceId>/system` (retained)
- `device/<deviceId>/status` (retained, LWT)

### Payload-Beispiele

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

Hinweis: In diesem Build ist MQTT-TLS nicht aktiv (`ASYNC_TCP_SSL_ENABLED=0`, AsyncMqttClient ohne `setSecure()` in der verwendeten Version).

## OTA Update

### Voraussetzungen

In `/config.json`:

- `ota.enabled = true`
- `ota.allowInsecureHttp = true` (expliziter Opt-in, da REST ueber HTTP laeuft)
- `security.enabled = true`
- `security.restToken` gesetzt

### Sicherheits-/Validierungsregeln

- OTA akzeptiert nur `Authorization: Bearer <token>`.
- Upload-Datei muss auf `.bin` enden.
- `Content-Length` ist Pflicht.
- Firmwaregroesse wird gegen OTA-Partition geprueft.
- Optionaler Integritaetscheck ueber Header `X-OTA-MD5`.
- Standard: Downgrade und gleiche Version blockiert (`ota.allowDowngrade=false`).

### Upload-Beispiel

```bash
NODE_IP=192.168.1.50
TOKEN="<dein-token>"

curl -i \
  -H "Authorization: Bearer $TOKEN" \
  -F "firmware=@.pio/build/esp32s3/firmware.bin" \
  "http://$NODE_IP/api/v1/ota"
```

Erfolg:

- `HTTP 200` mit `{"status":"ok","reboot":true}`
- Device rebootet automatisch.

### OTA Health-Confirm

Nach einem OTA-bootenden Image wird die Firmware erst nach einer Health-Phase bestaetigt:

- Wartezeit: `ota.healthConfirmMs`
- Optionaler Netzwerk-Gate: `ota.requireNetworkForConfirm`
- Bei ausbleibender Netzverfuegbarkeit innerhalb des erweiterten Fensters wird ein Rollback angefordert.

## Persistenz auf SD

Typische Dateien:

- `/config.json` (optional, manuell)
- `/history.jsonl` (Messhistorie)
- `/stats.json` (persistente Betriebszaehler)
- `/log-YYYYMMDD.log` oder `/log-uptime.log` (Logs)

### Logformat

```text
YYYY-MM-DD HH:MM:SS,mmm;LEVEL;Message
```

Bei fehlender NTP-Zeit werden Uptime-basierte Zeitstempel verwendet.

## Smoke Test

Die Hardware-Checkliste fuer Abnahme/Regressionen steht in [`SMOKETEST.md`](SMOKETEST.md).

## Troubleshooting

### `pio run` scheitert bei Libs

- Sauber neu bauen:

```bash
pio run -t clean
pio run
```

- Sicherstellen, dass Internetzugriff fuer `lib_deps` vorhanden ist.

### Keine REST-Erreichbarkeit

- Serielle Logs auf `ETH got IP` pruefen.
- IP aus Log verwenden.
- Bei `security.enabled=true` Auth-Header mitsenden.

### `/api/v1/health` ist `degraded`

- Erwartbar, wenn `mqtt.enabled=true` und Broker nicht verbunden ist.
- Broker/Netzwerkdaten in `mqtt.*` pruefen.

### OTA Endpoint nicht verfuegbar

- Pruefen, ob alle OTA-Voraussetzungen gesetzt sind (`ota.enabled`, `allowInsecureHttp`, `security.enabled`, `restToken`).

### Keine Sensorwerte

- DS18B20 Verdrahtung/Pull-up pruefen.
- 1-Wire auf GPIO17.
- Log meldet `no DS18B20 found`, wenn keine Geraete erkannt wurden.

## Entwicklung

### Relevante Dateien

- [`src/main.cpp`](src/main.cpp): Boot + Loop + Modulverdrahtung
- [`src/RestServer.cpp`](src/RestServer.cpp): REST API inkl. OTA Upload
- [`src/MqttClientManager.cpp`](src/MqttClientManager.cpp): MQTT + Reconnect + Buffer
- [`src/SensorManager.cpp`](src/SensorManager.cpp): DS18B20 Mess-State-Machine
- [`src/OtaHealthManager.cpp`](src/OtaHealthManager.cpp): OTA Health-Confirm / Rollback-Logik
- [`config.example.json`](config.example.json): Konfigurationsvorlage
- [`SMOKETEST.md`](SMOKETEST.md): Testcheckliste

### Nützliche Befehle

```bash
pio run
pio run -t upload
pio device monitor
```

## Lizenz

Aktuell ist keine Lizenzdatei hinterlegt. Fuer ein oeffentliches Repository sollte eine passende `LICENSE` ergaenzt werden.
