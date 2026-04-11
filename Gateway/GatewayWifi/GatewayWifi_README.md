# GatewayWifi — Zephyr BLE+WiFi Gateway (ESP32-WROVER)

A Zephyr RTOS application for the **ESP32-WROVER** that acts as a BLE Central gateway,
collecting sensor telemetry from up to 4 peripheral nodes and forwarding it to
**Azure IoT Hub** over MQTT via a plaintext Oracle Cloud bridge.

---

## What it does

1. Connects to a 2.4 GHz WPA2 WiFi network and obtains a DHCP lease
2. Connects to an Oracle Cloud MQTT bridge (plaintext, port 1883) which forwards to Azure IoT Hub over TLS server-side
3. Syncs UTC time via SNTP (`pool.ntp.org`) and distributes it to sensor nodes over BLE GATT WRITE
4. Scans for and connects to up to **4** BLE sensor nodes simultaneously
5. Receives two notification types per node:
   - **Sensor characteristic** (61 bytes): BME280 + ENS160 + AS7343 + sound summary + soil VWC
   - **Sound spectrum characteristic** (3 × ~236 byte chunks): 348-bin FFT, reassembled from 3 packets
6. Decodes binary BLE payloads, encodes structured JSON, and publishes to Azure IoT Hub
7. Handles MQTT keepalive and reconnects automatically on disconnect

### JSON telemetry schema

```json
{
  "header": { "messageId", "gatewayId", "schemaVersion", "messageType" },
  "payload": {
    "deviceId", "timestamp", "uptime_ms", "proto_ver",
    "environment": { "temperature_c", "humidity_percent", "pressure_hpa", "eco2_ppm", "tvoc_ppb", "aqi", "batt_mV" },
    "spectrum":    { "AS7343_405nm" .. "AS7343_VISIBLE" },
    "sound":       { "rms_dbfs", "peak_freq_hz", "peak_mag" },
    "soil":        { "vwc_percent" }
  }
}
```

---

## Hardware

- **ESP32-WROVER** (8 MB SPIRAM required — stacks and large buffers placed in `.ext_ram.bss`)
- Up to 4 × BLE sensor nodes advertising the tracker service UUID

---

## Project structure

```
GatewayWifi/
├── CMakeLists.txt
├── Kconfig                      # Not required for WiFi build
├── prj.conf                     # Zephyr Kconfig — WiFi, BT, MQTT, SNTP
├── app_dram1.ld                 # Custom linker fragment for DRAM/SPIRAM placement
├── src/
│   ├── main.c                   # Boot sequence — WiFi → MQTT → SNTP → BLE
│   ├── wifi.c                   # WiFi connection, DHCP, retry logic
│   └── wifi.h
└── common/  (shared, via CMakeLists.txt)
    ├── include/
    │   ├── azure_mqtt.h         # Azure/MQTT config + public API
    │   ├── bluetooth.h          # BLE stack sizes, MAX_CONN, thread declarations
    │   ├── my_json.h            # Payload structs + JSON API
    │   ├── sntp_sync.h
    │   ├── time_sync_writer.h
    │   └── env.h                # WiFi credentials (keep out of version control)
    └── src/
        ├── azure_mqtt.c         # MQTT client, message queue, publish thread
        ├── bluetooth.c          # BLE Central — scan, connect, notify, JSON publish
        ├── my_json.c            # JSON encoding for sensor + sound payloads
        ├── sntp_sync.c          # SNTP thread — syncs UTC every 60 s
        └── time_sync_writer.c   # Writes UTC to sensor nodes via GATT WRITE
```

---

## Boot sequence

```
main()
  │
  ├─ ethernet/wifi thread  →  IP obtained (blocks until lease)
  ├─ azure_mqtt_thread     →  connects to Oracle MQTT bridge
  ├─ k_sleep(3s)
  ├─ sntp_sync_start()     →  queries pool.ntp.org, sets UTC reference
  │
  └─ [15 s delay via K_THREAD_DEFINE]
       ├─ base_thread        →  bt_enable → scan → connect → subscribe
       └─ process_data_thread → dequeue → JSON encode → azure_mqtt_publish
```

---

## Configuration

### `common/include/env.h`

Keep this out of version control:

```c
#define CONFIG_WIFI_DEFAULT_SSID  "your-ssid"
#define CONFIG_WIFI_DEFAULT_PSK   "your-password"
```

### `common/include/azure_mqtt.h`

```c
#define AZURE_IOT_HUB_HOSTNAME  "161.33.232.177"   // Oracle bridge IP
#define AZURE_DEVICE_ID         "esp32-device-01"
#define AZURE_MQTT_PORT         1883                // Plaintext — no TLS on device
```

> **Production note:** `AZURE_ROOT_CA_CERT` and `AZURE_SAS_TOKEN` are defined but
> unused. They are placeholders for a future direct TLS connection from a WROVER.
> The current setup routes through the Oracle Cloud bridge which handles TLS server-side.

### Key `prj.conf` values

| Symbol | Value | Notes |
|---|---|---|
| `CONFIG_BT_MAX_CONN` | 4 | Match `MAX_CONN` in `bluetooth.h` |
| `CONFIG_MQTT_KEEPALIVE` | 10 | Seconds |
| `CONFIG_ESP_SPIRAM` | y | Required — buffers placed in SPIRAM |

---

## Memory layout

The WROVER's 8 MB SPIRAM is used to relieve the 96 KB internal DRAM shared
with BT and MQTT stacks:

| Symbol | Size | Placement |
|---|---|---|
| `process_stack` | 6144 B | `.ext_ram.bss` |
| `sntp_stack` | 2048 B | `.ext_ram.bss` |
| `s_sound_json` | 3072 B | `.ext_ram.bss` |
| `s_sensor_json` | 1600 B | `.ext_ram.bss` |
| `json_output` (my_json.c) | 1600 B | `.ext_ram.bss` |
| `s_sensor_pkt`, `s_jp`, `s_sound_pkt` | ~1 KB | `.ext_ram.bss` |

---

## Common issues

**DRAM overflow at link time**
Ensure `CONFIG_ESP_SPIRAM=y` is in `prj.conf` and the correct board target
(`esp32_wrover/esp32/procpu`) is used. Wrong board = SPIRAM guards don't fire.

**MQTT queue full — message dropped**
Sound packets arrive faster than they publish. Only 1 in 10 sound frames is
published by design (`sound_pkt_count >= 10` in `bluetooth.c`).

**CONNACK timeout**
WiFi must be fully up before the MQTT thread starts. The boot sequence enforces
this via `k_thread_join()` on the WiFi thread.

**BLE nodes not found**
Nodes must advertise the tracker service UUID. Check UUID bytes in `bluetooth.c`
match your sensor firmware.

**SNTP fails**
DNS must be working — SNTP failure retries every 10 s. UTC distribution to nodes
is skipped until a valid sync occurs (`time_sync_writer_has_utc()` returns false).
