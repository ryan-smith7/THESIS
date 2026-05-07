# Oracle Cloud MQTT Bridge — ESP32 to Azure IoT Hub

## Overview

This bridge solves two hardware limitations on the ESP32-WROOM module:

**Problem 1 — Memory:** The WROOM does not have enough SRAM to run Bluetooth
and TLS simultaneously. BT host pools consume ~56KB of dram1, leaving
insufficient heap for the MbedTLS TLS handshake (~30KB peak required).

**Problem 2 — Net buffer size:** The Zephyr net stack TX buffers must be
large enough to hold a full MQTT PUBLISH packet. With small buffers
(128 bytes × 6 = 768 bytes total), packets larger than ~100 bytes are
silently dropped by the net stack — the packet never leaves the ESP32.

**Solution:** An Oracle Cloud Always Free VM runs a Python MQTT bridge:
- Accepts **plaintext MQTT** from the ESP32 on port 1883 (no TLS on device)
- Forwards messages to **Azure IoT Hub over TLS** (TLS handled by the VM)

```
ESP32-POE (plaintext MQTT)
    ↓  port 1883
Oracle Cloud VM — 161.33.232.177
    ↓  port 8883 TLS
Azure IoT Hub — iot-hub-esp32-Ryan-Smith.azure-devices.net
```

**SD card upload path (browser → relay → IoT Hub):**
```
sensor_log_converter.html (browser)
    ↓  HTTP POST port 8080
upload_relay.py — 161.33.232.177:8080
    ↓  MQTT port 1883 (local)
Mosquitto → bridge.py
    ↓  port 8883 TLS
Azure IoT Hub
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
| Open Ports | 22 (SSH), 1883 (MQTT plaintext), 8080 (upload relay HTTP) |

---

## Files on VM

| File | Purpose |
|------|---------|
| `/home/ubuntu/bridge.py` | Python MQTT bridge — ESP32 → Azure IoT Hub |
| `/home/ubuntu/upload_relay.py` | HTTP relay — browser SD upload → Mosquitto |
| `/home/ubuntu/start_bridge.sh` | Shell wrapper — runs bridge.py with logging |
| `/home/ubuntu/bridge.log` | Bridge runtime log |
| `/etc/systemd/system/mqtt-bridge.service` | Systemd service — auto-starts bridge.py on boot |
| `/etc/systemd/system/upload-relay.service` | Systemd service — auto-starts upload_relay.py on boot |
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
# Start
sudo systemctl start mqtt-bridge

# Stop
sudo systemctl stop mqtt-bridge

# Restart
sudo systemctl restart mqtt-bridge

# Status
sudo systemctl status mqtt-bridge

# Live log
tail -f /home/ubuntu/bridge.log
```

### Upload Relay (upload_relay.py)

```bash
# Start
sudo systemctl start upload-relay

# Stop
sudo systemctl stop upload-relay

# Restart
sudo systemctl restart upload-relay

# Status
sudo systemctl status upload-relay

# Live log
sudo journalctl -u upload-relay -f
```

### Mosquitto

```bash
# Status
sudo systemctl status mosquitto

# Restart
sudo systemctl restart mosquitto

# Watch connection events
sudo tail -f /var/log/mosquitto/mosquitto.log
```

### Start / Stop All Services

```bash
# Start everything
sudo systemctl start mosquitto mqtt-bridge upload-relay

# Stop everything
sudo systemctl stop mqtt-bridge upload-relay

# Restart everything
sudo systemctl restart mosquitto mqtt-bridge upload-relay

# Status of all
sudo systemctl status mosquitto mqtt-bridge upload-relay
```

---

## Setting Up upload-relay as a Systemd Service

Run once to register the relay as a permanent service:

```bash
sudo tee /etc/systemd/system/upload-relay.service << 'EOF'
[Unit]
Description=SD Upload Relay — browser HTTP to Mosquitto MQTT
After=network.target mosquitto.service
Requires=mosquitto.service

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

sudo systemctl daemon-reload
sudo systemctl enable upload-relay
sudo systemctl start upload-relay
sudo systemctl status upload-relay
```

---

## Testing the Bridge

### Test ESP32 → IoT Hub (existing path)
```bash
mosquitto_pub -h 161.33.232.177 -p 1883 \
  -t "devices/esp32-device-01/messages/events/" \
  -m '{"test":"hello"}'

tail -f /home/ubuntu/bridge.log
```

### Test browser SD upload relay
```bash
curl -X POST http://161.33.232.177:8080/upload \
  -H "Content-Type: application/json" \
  -d '{"deviceId":"dev-1","utc_sec":1777000000,"utc_ms":0,"bme280":{"temperature_c":23.5,"humidity_percent":65.0,"pressure_hpa":101.3}}'
```

Expected response: `{"ok": 1}`

---

## ESP32 Configuration

### `azure_mqtt.h`
```c
/* Oracle Cloud VM — plaintext bridge */
#define AZURE_IOT_HUB_HOSTNAME   "161.33.232.177"
#define AZURE_MQTT_PORT          1883
```

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

## SAS Token Expiry

The current SAS token expires: **Mon Mar 23 2026**

To regenerate:
```bash
az iot hub generate-sas-token \
  --hub-name iot-hub-esp32-Ryan-Smith \
  --device-id esp32-device-01 \
  --duration 31536000
```

Update in `/home/ubuntu/bridge.py`:
```python
AZURE_PASSWORD = "SharedAccessSignature sr=..."
```

Then restart:
```bash
sudo systemctl restart mqtt-bridge
```

**Note:** The SAS token in `bridge.py` is the only place credentials live.
The browser upload tool (`sensor_log_converter.html`) has no credentials —
it posts to the relay which uses `bridge.py`'s token automatically.

---

## Production Upgrade Path

For production deployment, replace ESP32-WROOM with ESP32-WROVER (4MB PSRAM).
PSRAM absorbs BT host pools, freeing dram1 for MbedTLS. Direct TLS to Azure
then works without the Oracle bridge.

See bridge.py comments for TLS restoration steps.