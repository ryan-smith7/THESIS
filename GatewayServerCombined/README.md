# GatewayServer — Zephyr ESP32 → Azure IoT Hub

A minimal Zephyr RTOS application for the **ESP32** that connects to a WiFi network and publishes a structured JSON telemetry payload to **Azure IoT Hub** over **MQTT/TLS** every 30 seconds.

---

## What it does

1. Connects the ESP32 to a 2.4 GHz WPA2 WiFi network
2. Obtains an IP address via DHCP
3. Resolves your Azure IoT Hub hostname via DNS
4. Opens a TLS 1.2 connection to Azure IoT Hub on port **8883**
5. Authenticates using a **SAS Token** (symmetric key)
6. Publishes a JSON telemetry message to the D2C topic every 30 seconds
7. Handles keepalive (PINGREQ/PINGRESP) and reconnects automatically on disconnect

The telemetry payload schema matches:

```json
{
  "header": { "messageId", "gatewayId", "schemaVersion", "messageType" },
  "payload": { "deviceId", "timestamp", "uptime", "location", "environment", "acceleration" },
  "signature": { "alg", "keyId", "value" }
}
```

Sensor values in `build_telemetry_json()` inside `azure_mqtt.c` are currently hardcoded placeholders — replace them with live driver readings once your sensors are wired up.

---

## Project structure

```
.
├── CMakeLists.txt
├── prj.conf                  # Zephyr Kconfig — WiFi, MQTT, MbedTLS
├── DigiCertGlobalRootG2.pem  # Root CA for Azure IoT Hub TLS
├── include/
│   ├── azure_mqtt.h          # Azure config + public API
│   ├── wifi.h                # WiFi public API
│   └── env.h                 # Credentials (YOU MUST CREATE THIS)
└── src/
    ├── main.c                # Entry point — starts WiFi then MQTT thread
    ├── wifi.c                # WiFi connection, DHCP, retry logic
    └── azure_mqtt.c          # MQTT/TLS client, JSON builder, publish loop
```

---

## Prerequisites

- [Zephyr SDK 0.17+](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) with `west` installed
- ESP32 toolchain (`xtensa-espressif_esp32_zephyr-elf`)
- An **Azure IoT Hub** instance with a registered device
- Azure CLI (`az`) for generating SAS tokens

---

## Setup — step by step

### 1. Create `include/env.h`

This file is excluded from version control. Create it manually:

```c
#ifndef ENV_H_
#define ENV_H_

#define CONFIG_WIFI_DEFAULT_SSID   "your-wifi-ssid"
#define CONFIG_WIFI_DEFAULT_PSK    "your-wifi-password"

#endif /* ENV_H_ */
```

> **Do not commit this file to a public repository.**

---

### 2. Register your device in Azure IoT Hub

If you haven't already:

```bash
az iot hub device-identity create \
    --hub-name YOUR_HUB \
    --device-id dev-1
```

---

### 3. Generate a SAS Token

```bash
az iot hub generate-sas-token \
    --hub-name YOUR_HUB \
    --device-id dev-1 \
    --duration 2592000
```

`--duration` is in seconds. `2592000` = 30 days. Copy the full
`SharedAccessSignature sr=...` string.

---

### 4. Configure `include/azure_mqtt.h`

Fill in the three placeholders at the top of the file:

```c
#define AZURE_IOT_HUB_HOSTNAME   "YOUR_HUB.azure-devices.net"
#define AZURE_DEVICE_ID          "dev-1"
#define AZURE_SAS_TOKEN          "SharedAccessSignature sr=..."
```

---

### 5. Embed the DigiCert root CA certificate

Azure IoT Hub requires **DigiCert Global Root G2** for TLS. Verify your
`DigiCertGlobalRootG2.pem` is legitimate:

```bash
openssl x509 -in DigiCertGlobalRootG2.pem -noout -fingerprint -sha256
```

Expected SHA-256 fingerprint:
```
CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
```

Convert to a C string and paste it into `AZURE_ROOT_CA_CERT` in `azure_mqtt.h`:

```bash
awk '{printf "\"%s\\n\"\\\n", $0}' DigiCertGlobalRootG2.pem
```

---

### 6. Build

```bash
west build -b esp32_devkitc/esp32/procpu .
```

---

### 7. Flash

```bash
west flash
```

---

### 8. Monitor serial output

```bash
west espressif monitor
# or
screen /dev/ttyUSB0 115200
```

A successful run looks like:

```
[INF] main: === Gateway starting ===
[INF] wifi_module: WiFi thread started
[INF] wifi_module: Connecting to SSID: your-ssid
[INF] wifi_module: WiFi connected
[INF] wifi_module: IP: 192.168.1.42
[INF] wifi_module: IP address obtained — WiFi ready
[INF] main: Starting Azure MQTT thread
[INF] azure_mqtt: Resolving YOUR_HUB.azure-devices.net ...
[INF] azure_mqtt: DNS resolved
[INF] azure_mqtt: MQTT connecting to YOUR_HUB.azure-devices.net:8883 ...
[INF] azure_mqtt: CONNACK: connected to Azure IoT Hub
[INF] azure_mqtt: Published 512 bytes
```

---

## Verifying messages reach Azure

```bash
az iot hub monitor-events \
    --hub-name YOUR_HUB \
    --device-id dev-1
```

You should see your JSON payload arrive within 30 seconds of connection.

---

## Key configuration values

| Parameter | Location | Default |
|---|---|---|
| WiFi SSID / PSK | `include/env.h` | — |
| IoT Hub hostname | `include/azure_mqtt.h` | — |
| SAS Token | `include/azure_mqtt.h` | — |
| Publish interval | `azure_mqtt.c` `PUBLISH_INTERVAL_MS` | 30 000 ms |
| MQTT keepalive | `prj.conf` `CONFIG_MQTT_KEEPALIVE` | 60 s |
| MbedTLS heap | `prj.conf` `CONFIG_MBEDTLS_HEAP_SIZE` | 60 000 bytes |

---

## Common issues

**Build fails — Kconfig warnings treated as errors**
Zephyr 4.x is strict about undefined Kconfig symbols. Do not add
`CONFIG_MBEDTLS_*` cipher flags manually — they are selected automatically.

**TLS handshake fails at runtime**
- Wrong or expired root CA cert → re-check the SHA-256 fingerprint above
- Expired SAS Token → regenerate with `az iot hub generate-sas-token`
- Insufficient MbedTLS heap → increase `CONFIG_MBEDTLS_HEAP_SIZE` in `prj.conf`

**CONNACK timeout**
- DNS failed — check WiFi is connected and `CONFIG_DNS_RESOLVER=y` is set
- Port 8883 blocked — Azure IoT Hub **only** accepts TLS on 8883; plain 1883 is rejected

**WiFi won't connect**
- Confirm the SSID is 2.4 GHz (ESP32 does not support 5 GHz)
- Check credentials in `env.h`
- The retry logic will attempt up to `WIFI_RETRY_COUNT` (10) times with exponential backoff

---

## Extending this project

- **Live sensor data** — replace the hardcoded strings in `build_telemetry_json()` in `azure_mqtt.c` with values from your sensor drivers
- **Receiving commands** — subscribe to `devices/dev-1/messages/devicebound/#` in `azure_mqtt_connect()` and handle `MQTT_EVT_PUBLISH` in the event callback
- **SAS Token rotation** — store the token in NVS (enable `CONFIG_SETTINGS_NVS`) and refresh it before expiry
- **Longer SAS Token lifetime** — pass `--duration 31536000` (1 year) to the `az` CLI command for development devices
