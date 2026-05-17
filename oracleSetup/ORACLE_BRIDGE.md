# Oracle Cloud MQTT Bridge — ESP32 to Azure SQL

## Overview

This bridge solves two hardware limitations on the ESP32-WROOM module:

**Problem 1 — Memory:** The WROOM does not have enough SRAM to run Bluetooth
and TLS simultaneously. BT host pools consume ~56KB of dram1, leaving
insufficient heap for the MbedTLS TLS handshake (~30KB peak required).

**Problem 2 — Net buffer size:** The Zephyr net stack TX buffers must be
large enough to hold a full MQTT PUBLISH packet. With small buffers
(128 bytes × 6 = 768 bytes total), packets larger than ~100 bytes are
silently dropped by the net stack — the packet never leaves the ESP32.

**Solution:** An Oracle Cloud Always Free VM runs three Python services:
- Accepts **plaintext MQTT** from the ESP32 on port 1883 (no TLS on device)
- Forwards messages **directly to Azure SQL** via upload_relay.py (IoT Hub bypassed)
- Serves **SNTP-over-HTTP time sync** on port 80 for ESP32 UTC synchronisation

### Live telemetry path
```
ESP32 (plaintext MQTT)
    ↓  port 1883
Mosquitto — localhost
    ↓  subscribe devices/#
bridge.py
    ↓  HTTP POST localhost:8080/upload_csv
upload_relay.py
    ↓  pyodbc MERGE
Azure SQL — iot-telemetry-db
```

### Browser backfill path (SD card upload)
```
sensor_log_converter.html (browser)
    ↓  HTTP POST port 8080/upload_csv
upload_relay.py
    ↓  pyodbc MERGE
Azure SQL — iot-telemetry-db
```

### Time sync path
```
ESP32 http_time_sync thread
    ↓  TCP GET /time port 80
time_server.py — 161.33.232.177:80
    ↓  12-byte binary response (t2, t3 timestamps)
ESP32 computes SNTP offset θ = ((t2−t1) + (t3−t4)) / 2
```

---

## Infrastructure

| Resource | Details |
|----------|---------|
| Cloud Provider | Oracle Cloud Infrastructure (Always Free) |
| VM Shape | VM.Standard.E2.1.Micro (1 OCPU, 1GB RAM) |
| OS | Ubuntu 22.04 LTS |
| Public IP | 161.33.232.177 |
| VCN | vcn-mqtt (10.0.0.0/16) |
| Subnet | subnet-public (10.0.0.1/24) |
| Open Ports | 22 (SSH), 1883 (MQTT plaintext), 8080 (upload relay), 80 (time server) |

---

## Files on VM

| File | Purpose |
|------|---------|
| `/home/ubuntu/bridge.py` | MQTT bridge — Mosquitto → upload_relay /upload_csv |
| `/home/ubuntu/upload_relay.py` | HTTP relay — MERGE inserts to Azure SQL |
| `/home/ubuntu/time_server.py` | SNTP-over-HTTP time server — port 80 |
| `/etc/systemd/system/mqtt-bridge.service` | Systemd service — bridge.py |
| `/etc/systemd/system/upload-relay.service` | Systemd service — upload_relay.py |
| `/etc/systemd/system/time_server.service` | Systemd service — time_server.py |
| `/etc/systemd/system/midnight-restart.timer` | Daily restart timer — 13:05 UTC (23:05 AEST) |
| `/etc/mosquitto/conf.d/listener.conf` | Mosquitto plaintext listener on port 1883 |

---

## SSH Access

```bash
ssh -i ~/.ssh/oracle_vm ubuntu@161.33.232.177
```

Private key location on Mac: `~/.ssh/oracle_vm`

---

## Service Management

### MQTT Bridge (bridge.py)

```bash
sudo systemctl start mqtt-bridge
sudo systemctl stop mqtt-bridge
sudo systemctl restart mqtt-bridge
sudo systemctl status mqtt-bridge
sudo journalctl -u mqtt-bridge -f
```

### Upload Relay (upload_relay.py)

```bash
sudo systemctl start upload-relay
sudo systemctl stop upload-relay
sudo systemctl restart upload-relay
sudo systemctl status upload-relay
sudo journalctl -u upload_relay -f
```

### Time Server (time_server.py)

```bash
sudo systemctl start time_server
sudo systemctl stop time_server
sudo systemctl restart time_server
sudo systemctl status time_server
sudo journalctl -u time_server -f
```

### Mosquitto

```bash
sudo systemctl status mosquitto
sudo systemctl restart mosquitto
sudo tail -f /home/ubuntu/bridge.log
```

### Restart All Services

```bash
sudo systemctl restart mosquitto mqtt-bridge upload-relay time_server
```

---

## Setting Up Services as Systemd Units

### upload-relay

```bash
sudo tee /etc/systemd/system/upload-relay.service << 'EOF'
[Unit]
Description=SD Upload Relay — HTTP to Azure SQL
After=network.target mosquitto.service

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu
ExecStart=/usr/bin/python3 /home/ubuntu/upload_relay.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
```

### time_server

```bash
sudo tee /etc/systemd/system/time_server.service << 'EOF'
[Unit]
Description=SNTP-over-HTTP time server
After=network.target

[Service]
User=ubuntu
ExecStart=/usr/bin/python3 /home/ubuntu/time_server.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
```

### midnight-restart timer

```bash
sudo tee /etc/systemd/system/midnight-restart.service << 'EOF'
[Unit]
Description=Midnight restart of bridge and relay

[Service]
Type=oneshot
ExecStart=/bin/systemctl restart mqtt-bridge
ExecStart=/bin/systemctl restart upload-relay
EOF

sudo tee /etc/systemd/system/midnight-restart.timer << 'EOF'
[Unit]
Description=Daily restart at 23:05 AEST (13:05 UTC)

[Timer]
OnCalendar=*-*-* 13:05:00
Persistent=true

[Install]
WantedBy=timers.target
EOF
```

Enable all:

```bash
sudo systemctl daemon-reload
sudo systemctl enable upload-relay time_server midnight-restart.timer
sudo systemctl start upload-relay time_server midnight-restart.timer
```

---

## Testing

### Live telemetry (MQTT → SQL)
```bash
mosquitto_pub -h localhost -p 1883 \
  -t "devices/dev-1/messages/events/" \
  -m '{"deviceId":"dev-1","utc_sec":1778000000,"utc_ms":0,"bme280":{"temperature_c":23.5,"humidity_percent":65.0,"pressure_hpa":101.3}}'

sudo journalctl -u mqtt-bridge -f
```

### Browser backfill upload
```bash
curl -X POST http://161.33.232.177:8080/upload_csv \
  -H "Content-Type: application/json" \
  -d '{"modality":"bme280","rows":[{"device_id":"dev-1","datetime":"2026-05-14 12:00:00.000","temp_c":23.5,"rh_pct":65.0,"press_hPa":101.3}]}'
```

Expected response: `{"ok": 1}`

### Time server health
```bash
curl http://161.33.232.177:80/health
```

Expected response: `OK 1778759375.122`

---

## Supported Modalities

| Modality key | Table | Device | Description |
|---|---|---|---|
| `bme280` | telemetry_bme280 | dev-1 | Temperature, humidity, pressure |
| `ens160` | telemetry_ens160 | dev-1 | eCO2, TVOC, AQI |
| `sound` | telemetry_sound | dev-1 | RMS dBFS + 348-bin FFT spectrum |
| `as7343` | telemetry_spectrum | dev-2 | 13-channel light spectrum |
| `moisture` | telemetry_soil | dev-2 | Volumetric water content |
| `ds18b20` | telemetry_soil_temperature | dev-2 | Soil temperature |
| `battery` | telemetry_battery | dev-1/2 | mV, %, charge rate |

All inserts use `MERGE ON (device_id, timestamp)` for deduplication.
Each table has a `UNIQUE INDEX` on `(device_id, timestamp)` —
`telemetry_sound` uses `(device_id, received_time)`.

---

## Time Sync (SNTP-over-HTTP)

The ESP32 cannot use standard SNTP (UDP port 123 is blocked). Instead,
`time_server.py` implements the RFC 4330 §5 offset formula over HTTP/TCP on port 80.

**Protocol:**
- ESP32 sends `GET /time HTTP/1.0`
- Server records `t2` (request arrival) and `t3` (response send) as UTC timestamps
- Response body: 12 bytes big-endian — `uint32 t2_sec`, `uint16 t2_ms`, `uint32 t3_sec`, `uint16 t3_ms`
- ESP32 records `t1` (uptime before send) and `t4` (uptime after receive)
- Offset: `θ = ((t2 − t1) + (t3 − t4)) / 2`
- Stored as `utc_offset_ms = UTC_ms − uptime_ms`; all subsequent time calls use `k_uptime_get() + utc_offset_ms`

---

## Azure SQL

| Resource | Details |
|----------|---------|
| Server | iot-telemetry-server-ryan-smith.database.windows.net |
| Database | iot-telemetry-db |
| User | sqladmin |
| Driver | ODBC Driver 18 for SQL Server |

Timestamps are stored as UTC. Grafana displays in browser local time (AEST = UTC+10).

---

## ESP32 Configuration

### `prj.conf` — critical settings

```properties
# MQTT — plaintext, no TLS
CONFIG_MQTT_LIB=y
CONFIG_MQTT_LIB_TLS=n
CONFIG_MQTT_KEEPALIVE=10

CONFIG_NET_BUF_TX_COUNT=16
CONFIG_NET_BUF_DATA_SIZE=512
```

---

## Production Upgrade Path

For production deployment, replace ESP32-WROOM with ESP32-WROVER (4MB PSRAM).
PSRAM absorbs BT host pools, freeing dram1 for MbedTLS. Direct TLS to Azure
then works without the Oracle bridge.