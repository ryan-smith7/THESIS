# GatewayEthernet — Zephyr BLE+Ethernet Gateway (ESP32-POE)

A Zephyr RTOS application for the **ESP32-POE** (Olimex) that acts as a BLE Central
gateway, collecting sensor telemetry from up to 2 peripheral nodes and forwarding it
to **Azure IoT Hub** over MQTT via a plaintext Oracle Cloud bridge.

This is the wired counterpart to GatewayWifi. The public API and boot sequence are
identical — only the network transport and hardware constraints differ.

---

## What it does

1. Brings up the LAN8720 Ethernet PHY (RMII) and obtains a DHCP lease
2. Connects to an Oracle Cloud MQTT bridge (plaintext, port 1883) which forwards to Azure IoT Hub over TLS server-side
3. Syncs UTC time via SNTP (`pool.ntp.org`) and distributes it to sensor nodes over BLE GATT WRITE
4. Scans for and connects to up to **2** BLE sensor nodes simultaneously
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

- **ESP32-POE** (Olimex) — LAN8720 Ethernet PHY via RMII
- RJ45 connected to a DHCP-capable switch/router
- Up to 2 × BLE sensor nodes advertising the tracker service UUID

> **Why MAX_CONN 2 not 4?**
> The ESP32-POE has no SPIRAM. BT + Ethernet + MQTT share the 96 KB internal
> DRAM. Limiting to 2 connections keeps the per-connection BLE buffers,
> message queues, and working JSON buffers within budget.

---

## Project structure

```
GatewayEthernet/
├── CMakeLists.txt
├── Kconfig                      # Declares CONFIG_ETH_GATEWAY symbol
├── prj.conf                     # Zephyr Kconfig — Ethernet, BT, MQTT, SNTP
├── boards/
│   └── esp32_poe_procpu.overlay # DTS overlay — LAN8720 RMII pinmux
├── dts/bindings/sensor/
│   └── ams,as7343.yaml
├── src/
│   ├── main.c                   # Boot sequence — Ethernet → MQTT → SNTP → BLE
│   ├── ethernet.c               # Ethernet init, DHCP, net-mgmt callbacks
│   └── ethernet.h
└── common/  (shared, via CMakeLists.txt)
    ├── include/
    │   ├── azure_mqtt.h         # Azure/MQTT config + public API
    │   ├── bluetooth.h          # BLE stack sizes, MAX_CONN, thread declarations
    │   ├── my_json.h            # Payload structs + JSON API
    │   ├── sntp_sync.h
    │   ├── time_sync_writer.h
    │   └── env.h                # WiFi credentials (unused here, keep for common compatibility)
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
  ├─ ethernet thread   →  LAN8720 up → DHCP lease obtained (blocks)
  ├─ azure_mqtt_thread →  connects to Oracle MQTT bridge
  ├─ k_sleep(3s)
  ├─ sntp_sync_start() →  queries pool.ntp.org, sets UTC reference
  │
  └─ [15 s delay via K_THREAD_DEFINE]
       ├─ base_thread         →  bt_enable → scan → connect → subscribe
       └─ process_data_thread → dequeue → JSON encode → azure_mqtt_publish
```

---

## Configuration

### `common/include/azure_mqtt.h`

```c
#define AZURE_IOT_HUB_HOSTNAME  "161.33.232.177"   // Oracle bridge IP
#define AZURE_DEVICE_ID         "esp32-device-01"
#define AZURE_MQTT_PORT         1883                // Plaintext — no TLS on device
```

> **Production note:** `AZURE_ROOT_CA_CERT` and `AZURE_SAS_TOKEN` are defined but
> unused. The `root_ca.pem` file in this directory is also unused. Both are
> placeholders for a future direct TLS connection. The Oracle Cloud bridge
> handles TLS server-side.

### `Kconfig`

```kconfig
mainmenu "Gateway Ethernet Configuration"
source "Kconfig.zephyr"

config ETH_GATEWAY
    bool "Use Ethernet (ESP32-POE) instead of WiFi"
    default y
```

This defines `CONFIG_ETH_GATEWAY` which gates the platform-specific `#ifdef`
blocks throughout the common source files.

### Key `prj.conf` values

| Symbol | Value | Notes |
|---|---|---|
| `CONFIG_ETH_GATEWAY` | y | Selects Ethernet path in shared source |
| `CONFIG_BT_MAX_CONN` | 2 | Match `MAX_CONN` in `bluetooth.h` |
| `CONFIG_MQTT_KEEPALIVE` | 10 | Seconds |
| `CONFIG_ESP_SPIRAM` | not set | No SPIRAM on POE — all buffers in internal DRAM |

---

## Memory layout

No SPIRAM — all data in internal DRAM. Kept within budget by:

- `MAX_CONN 2` (vs 4 on WROVER) — halves per-connection BLE buffer allocations
- `BASE_CONTROL_STACK_SIZE 1536` (vs 2048)
- `BASE_PROCESS_STACK_SIZE 4096` (vs 6144)
- `process_stack 4096` (vs 6144)
- Working JSON buffers (`s_sound_json` 3072 B, `s_sensor_json` 1600 B) stay in BSS

If you hit DRAM overflow, reduce `CONFIG_BT_MAX_CONN` further or trim
`NET_BUF_RX_COUNT` / `NET_BUF_TX_COUNT` in `prj.conf`.

---

## Common issues

**DRAM overflow at link time**
Ensure you are building with `-b esp32_poe/esp32/procpu` not `esp32_devkitc`.
Wrong board = all Kconfig symbols undefined = build aborts.

**Ethernet link never comes up**
Check the DTS overlay is being applied (pass `--build-dir` or
`-DDTC_OVERLAY_FILE` as shown in `COMMANDS.md`). Verify the RJ45 is connected
to an active port with DHCP.

**DHCP times out**
`ethernet_thread` polls every 3 s indefinitely. Check switch/router is
assigning leases. The POE board requires the PHY reset GPIO to be configured
correctly in the overlay.

**CONNACK timeout**
Ethernet must be fully up before the MQTT thread starts. Enforced via
`k_thread_join()` on the Ethernet thread in `main.c`.

**BLE nodes not found**
Nodes must advertise the tracker service UUID. Check UUID bytes in `bluetooth.c`
match your sensor firmware.

**Kconfig warnings treated as errors**
You must use the correct board target. Building against `esp32_devkitc` leaves
all networking and BT symbols undefined, causing Zephyr 4.x to abort.
