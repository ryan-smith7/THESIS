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
| Open Ports | 22 (SSH), 1883 (MQTT plaintext) |

---

## Files on VM

| File | Purpose |
|------|---------|
| `/home/ubuntu/bridge.py` | Python MQTT bridge script |
| `/home/ubuntu/start_bridge.sh` | Shell wrapper — runs bridge.py with logging |
| `/home/ubuntu/bridge.log` | Bridge runtime log |
| `/etc/systemd/system/mqtt-bridge.service` | Systemd service — auto-starts on boot |
| `/etc/mosquitto/conf.d/listener.conf` | Mosquitto plaintext listener on port 1883 |
| `/etc/mosquitto/conf.d/azure_bridge.conf.disabled` | Disabled Mosquitto bridge (SSL issues) |

---

## SSH Access

```bash
ssh -i ~/.ssh/oracle_vm ubuntu@161.33.232.177
```

Private key location on Mac: `~/.ssh/oracle_vm`

---

## Service Management

Check bridge status:
```bash
sudo systemctl status mqtt-bridge
sudo systemctl status mosquitto
```

View live bridge log:
```bash
tail -f /home/ubuntu/bridge.log
```

Restart bridge:
```bash
sudo systemctl restart mqtt-bridge
sudo systemctl restart mosquitto
```

Watch Mosquitto connection events:
```bash
sudo tail -f /var/log/mosquitto/mosquitto.log
```

---

## Testing the Bridge

From any machine with Mosquitto installed:
```bash
mosquitto_pub -h 161.33.232.177 -p 1883 \
  -t "devices/esp32-device-01/messages/events/" \
  -m '{"test":"hello"}'
```

Watch bridge log for forwarding confirmation:
```bash
tail -f /home/ubuntu/bridge.log
```

Expected output:
```
INFO Forwarding [XX bytes]: devices/esp32-device-01/messages/events/...
INFO Azure publish OK mid=XX
```

Then check Azure IoT Hub portal → Monitoring → Metrics.

---

## ESP32 Configuration

### `azure_mqtt.h`
```c
/* Oracle Cloud VM — plaintext bridge */
#define AZURE_IOT_HUB_HOSTNAME   "161.33.232.177"
#define AZURE_MQTT_PORT          1883
```

### `azure_mqtt.c`
- Transport: `MQTT_TRANSPORT_NON_SECURE`
- No TLS config block
- No `tls_credentials_register()` call
- Message queue pattern — BLE thread enqueues, MQTT thread dequeues and publishes
- `tx_buf[4608]` — must be > max payload + MQTT header overhead
- MQTT thread priority 4 — higher than BLE threads (priority 5) to ensure
  keepalives are sent and the connection stays alive

### `prj.conf` — critical settings

```properties
# MQTT — plaintext, no TLS
CONFIG_MQTT_LIB=y
CONFIG_MQTT_LIB_TLS=n
CONFIG_MQTT_KEEPALIVE=10

# Network Buffers — sized for 600+ byte MQTT payloads
# ROOT CAUSE OF SILENT PUBLISH FAILURE:
# With DATA_SIZE=128 and TX_COUNT=6, total TX pool = 768 bytes.
# A 679-byte MQTT packet (74-byte topic + 600-byte payload + header)
# consumes all 6 buffers, leaving none for TCP ACKs.
# The net stack silently drops the packet — mqtt_publish() returns 0
# but the data never leaves the ESP32.
# Fix: DATA_SIZE=512 × TX_COUNT=16 = 8KB TX pool — plenty of headroom.
CONFIG_NET_TX_STACK_SIZE=1536
CONFIG_NET_RX_STACK_SIZE=1536
CONFIG_NET_MAX_CONTEXTS=6
CONFIG_NET_PKT_RX_COUNT=12
CONFIG_NET_PKT_TX_COUNT=8
CONFIG_NET_BUF_RX_COUNT=24
CONFIG_NET_BUF_TX_COUNT=16
CONFIG_NET_BUF_DATA_SIZE=512

# All MbedTLS/TLS disabled — not needed for plaintext bridge
# CONFIG_MQTT_LIB_TLS=n
# CONFIG_TLS_CREDENTIALS not set
# CONFIG_MBEDTLS not set
# Removing MbedTLS frees ~40KB dram1 for BT host pools

# Bluetooth
CONFIG_BT=y
CONFIG_BT_CENTRAL=y
CONFIG_BT_MAX_CONN=1
CONFIG_BT_MAX_PAIRED=0
CONFIG_BT_SMP=y
CONFIG_BT_BONDABLE=n
CONFIG_ESP32_REGION_1_NOINIT=y
```

---

## Net Buffer Math

```
MQTT PUBLISH packet for 600-byte sensor JSON:
  Fixed header:    5 bytes
  Topic string:   74 bytes  (devices/esp32-device-01/messages/events/...)
  Payload:       600 bytes
  ─────────────────────────
  Total:         679 bytes

With DATA_SIZE=128, TX_COUNT=6 (OLD — BROKEN):
  679 ÷ 128 = 6 buffers needed = entire pool exhausted
  Zero buffers left for TCP ACK → packet silently dropped

With DATA_SIZE=512, TX_COUNT=16 (NEW — WORKING):
  679 ÷ 512 = 2 buffers needed
  14 buffers remaining for TCP overhead → packet sent successfully
```

---

## Thread Priority Notes

Zephyr priority: lower number = higher priority.

| Thread | Priority | Notes |
|--------|----------|-------|
| BLE control (base_thread) | 5 | Scans and connects to sensor nodes |
| BLE process (process_data_thread) | 5 | Decodes packets, enqueues to MQTT |
| MQTT (azure_mqtt_thread) | 4 | **Must be higher than BLE** to send keepalives |
| Ethernet | 6 | DHCP, exits after IP obtained |
| SNTP | 6 | Periodic time sync |

If MQTT thread priority is lower than BLE threads, BLE starves the MQTT
thread preventing PINGREQs from being sent. Mosquitto times out the
connection at 1.5 × keepalive = 15 seconds.

---

## Message Queue Architecture

The BLE thread and MQTT thread cannot safely share the MQTT client struct.
A message queue decouples them:

```
BLE thread:   azure_mqtt_publish(json) → k_msgq_put() → returns immediately
MQTT thread:  k_msgq_get() → do_publish() → mqtt_live() → poll → repeat
```

The MQTT thread exclusively owns all mqtt_* calls. No mutex needed.
Queue depth = 2 messages. If queue is full, messages are dropped with a warning.

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

---

## Production Upgrade Path

For production deployment:

1. **Replace ESP32-WROOM with ESP32-WROVER** — 4MB PSRAM absorbs BT host
   pools, leaving dram1 free for MbedTLS heap. Direct TLS to Azure works.

2. **Restore TLS in `azure_mqtt.c`:**
   ```c
   client.transport.type = MQTT_TRANSPORT_SECURE;
   tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
   ```

3. **Restore in `prj.conf`:**
   ```properties
   CONFIG_MQTT_LIB_TLS=y
   CONFIG_TLS_CREDENTIALS=y
   CONFIG_MBEDTLS=y
   CONFIG_MBEDTLS_BUILTIN=y
   CONFIG_MBEDTLS_ENABLE_HEAP=y
   CONFIG_MBEDTLS_HEAP_SIZE=40960
   CONFIG_MBEDTLS_HEAP_CUSTOM_SECTION=y
   CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=4096
   CONFIG_MBEDTLS_TLS_VERSION_1_2=y
   CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED=y
   ```

4. **Change hostname back:**
   ```c
   #define AZURE_IOT_HUB_HOSTNAME "iot-hub-esp32-Ryan-Smith.azure-devices.net"
   #define AZURE_MQTT_PORT        8883
   ```

5. **Oracle VM bridge can be decommissioned.**

---

## Why Not TLS Directly on ESP32-WROOM

The ESP32-WROOM has 520KB SRAM:
- dram0: 140KB available (after 55KB BT controller HW reservation)
- dram1: 96KB total

With BT enabled:
- BT host noinit pools: 56KB → dram1
- MbedTLS heap (minimum): 40KB → dram1
- Total dram1: 96KB = 99.4% full — 592 bytes free

TLS 1.2 handshake with Azure (ECDHE-RSA + DigiCert G2 chain) needs
~30KB peak MbedTLS heap. The heap allocator fails due to fragmentation.
This is a confirmed known issue with BLE + TLS coexistence on ESP32-WROOM
— see arduino-esp32 issue #2175 and esp-idf issue #2171.

Every approach to work around this was exhausted:
- Custom dual-region MbedTLS heap across dram0+dram1 → dram0 overflow
- TLS_PEER_VERIFY_NONE → still needs ~10KB, still fails with BT pools
- Reducing BT buffers → insufficient for stable BLE connections
- Moving app buffers between regions → all combinations tried

The WROVER with 4MB PSRAM is the correct hardware for this application.
