# Hardware Smoke Test (ESP32-S3-ETH)

Diese Checkliste validiert Boot, Sensorik, REST, MQTT, SD-Logging und Persistenz auf echter Hardware.

## 1. Vorbereitung

1. Firmware bauen und flashen:
   ```bash
   pio run -t upload
   ```
2. Seriellen Monitor starten:
   ```bash
   pio device monitor
   ```
3. Optional `/config.json` auf SD anlegen (auf Basis von `config.example.json`).
4. Notiere:
   - IP-Adresse des Nodes (aus seriellen Logs)
   - mindestens eine Sensor-ID aus `/api/v1/temps`

## 2. Boot und Basisfunktion

1. Reset auslösen (Power-Cycle oder Reset-Taster).
2. Erwartung im Log:
   - `booting...`
   - `ETH start`
   - `ETH got IP: ...`
   - `setup complete`
3. Falls SD gesteckt:
   - keine dauerhaften `SD mount failed/unavailable`
4. Falls Sensor vorhanden:
   - wiederkehrende `temp update: ...`

## 3. REST-API prüfen

Beispiel mit `NODE_IP=192.168.1.50`:

```bash
NODE_IP=192.168.1.50
curl -sS "http://$NODE_IP/api/v1/health"
curl -sS "http://$NODE_IP/api/v1/system"
curl -sS "http://$NODE_IP/api/v1/metrics"
curl -sS "http://$NODE_IP/api/v1/temps"
curl -sS "http://$NODE_IP/api/v1/sensors"
curl -sS "http://$NODE_IP/api/v1/history?limit=10"
```

Erwartung:
- `health` liefert `{"status":"ok"}` oder bei fehlendem MQTT `degraded` (wenn MQTT aktiviert ist, aber nicht verbunden).
- `temps` enthält Sensorwerte oder leere Liste.
- `metrics.stats.bootCount` steigt nach Reboot.

## 4. Sensor-Intervall (REST) prüfen

```bash
NODE_IP=192.168.1.50
curl -sS "http://$NODE_IP/api/v1/sensors/interval"
curl -sS -X POST "http://$NODE_IP/api/v1/sensors/interval" -d "intervalMs=2000"
curl -sS -X PUT "http://$NODE_IP/api/v1/sensors/interval" -H "Content-Type: application/json" -d '{"intervalMs":5000}'
curl -sS "http://$NODE_IP/api/v1/sensors/interval"
```

Erwartung:
- `intervalMs` entspricht dem gesetzten Wert (geclamped auf 250..3600000).

## 5. MQTT prüfen

Voraussetzung: MQTT-Broker erreichbar, `mqtt.enabled=true`.

```bash
BROKER=192.168.1.10
TOPIC_BASE=device
DEVICE_ID=<deviceId>

mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/$DEVICE_ID/status" -v
mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/$DEVICE_ID/system" -v
mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/$DEVICE_ID/temps/+" -v
```

Erwartung:
- Status `online` nach Verbindung.
- Regelmäßige `system`-Publishes.
- Temperaturwerte je Sensor unter `temps/<sensorId>`.

## 6. SD-Logging und History prüfen

1. Gerät einige Minuten laufen lassen.
2. SD am Rechner prüfen:
   - `log-YYYYMMDD.log` oder `log-uptime.log`
   - `history.jsonl`
   - `stats.json`
3. Erwartung:
   - Logzeilen sind zeitlich sortiert.
   - `history.jsonl` enthält JSONL-Einträge mit `sensorId`, `tempC`, `timestamp`.

## 7. Security prüfen (wenn aktiviert)

In `/config.json`:
- `security.enabled=true`
- entweder `restToken` setzen oder `restUser`/`restPass` setzen

Tests:
```bash
NODE_IP=192.168.1.50
TOKEN=<token>
curl -i "http://$NODE_IP/api/v1/system"
curl -i -H "Authorization: Bearer $TOKEN" "http://$NODE_IP/api/v1/system"
```

Erwartung:
- Ohne Auth: `401`
- Mit korrekter Auth: `200`
- Query-Token (`?token=...`) wird nicht akzeptiert.

## 8. Static-IP prüfen

In `/config.json`:
- `network.dhcp=false`
- gültige `ip/gw/mask/dns`

Nach Reboot:
1. Serielle Logs prüfen (`ETH static IP configured: ...`).
2. REST unter statischer IP aufrufen.

## 9. OTA-Upload prüfen

Voraussetzungen in `/config.json`:
- `ota.enabled=true`
- `ota.allowInsecureHttp=true` (expliziter Opt-in)
- `ota.requireHashHeader=true` (Default)
- `security.enabled=true`
- `security.restToken` gesetzt

Upload-Test:
```bash
NODE_IP=192.168.1.50
TOKEN=<token>
SHA256=$(shasum -a 256 .pio/build/esp32s3/firmware.bin | awk '{print $1}')
curl -i -H "Authorization: Bearer $TOKEN" \
     -H "X-OTA-SHA256: $SHA256" \
     -F "firmware=@.pio/build/esp32s3/firmware.bin" \
     "http://$NODE_IP/api/v1/ota"
```

Erwartung:
- HTTP 200 mit `{"status":"ok","reboot":true}`
- Gerät startet neu
- Bei gleicher/älterer Version wird Upload abgewiesen (wenn `ota.allowDowngrade=false`)

## 10. Persistenz prüfen

1. Mehrere Reboots ausführen.
2. Prüfen:
   - `metrics.stats.bootCount` steigt konsistent.
   - `system.resetReason` plausibel.
   - keine wiederkehrenden SD-Schreibfehler.

## 11. Abnahmekriterien

- Build erfolgreich (`pio run`).
- REST-Endpunkte antworten stabil.
- Sensorwerte werden erzeugt.
- MQTT sendet (falls aktiviert).
- SD-Dateien werden geschrieben.
- Nach Reboot bleiben Stats konsistent.
