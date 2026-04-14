# Prac2 README File

## Author
**Ryan Smith**  
Student ID: 47444131

## Functionality Achieved

### Design Task 1 (Real Time Clock - RTC)
Implements a library for initializing and interfacing with the on-chip RTC to get/set the time in ISO 8601 format.

### Design Task 2 (Sensor Interfacing)
Threaded sensor library using Zephyr RTOS. Each sensor runs in its own thread, triggered by semaphores, and writes sampled data to a shared ring buffer.

- **HTS221 Thread** – Samples temperature and humidity.
- **LPS22HB Thread** – Samples pressure.
- **LIS3MDL Thread** – Samples 3-axis magnetometer data.
- **Sampling Thread** – Periodically collects selected sensor data, formats it as JSON, and outputs to the terminal. Logging to file is supported if enabled.

#### Sensor Types:
- `0`: Temperature
- `1`: Humidity
- `2`: Pressure
- `4`: Magnetometer
- `DID_ALL`: All sensors

### Design Task 3 (CLI Shell)
Implements a command-line interface using the Zephyr Shell library for:

#### Shell Commands:
- `sensor <DID>` – Read sensor values by DID
- `rtc r` – Reads and displays the current time in ISO 8601 format.
- `rtc w <YYYY-MM-DD> <HH:MM:SS>` – Set RTC time.

### Design Task 4 (Continuous Sampling)
Implements continuous sampling functionality triggered via shell or pushbutton. Outputs sensor data in JSON format using Zephyr's JSON libraries.

#### Shell Command:
- `sample s <DID>` – Start sampling for specified sensor.
- `sample p <DID>` – Stop sampling.
- `sample w <rate>` – Set sampling rate in seconds.

#### Output Format:
```json
{ 
  "did": 255, 
  "time": "2025-04-06T14:03:00", 
  "values": [22.5, 55.2, 1004.3, 30.1, -5.0, 18.3] 
}
```
### Button Control:
Physical pushbutton also used to start/stop continuous sampling.

### Design Task 5 (File System Integration)
Implements the Zephyr File System using the littleFs file system mounted, with the following CLI commands:

#### CLI Functions:
- `ls_cmd`: Lists files and directories in the current or specified directory.
- `cd_cmd`: Changes the current working directory.
- `mkdir_cmd`: Creates a new directory.
- `write_cmd`: Writes data to a specified file.
- `read_cmd`: Reads and displays the contents of a file.
- `pwd_cmd`: Prints the current working directory.
- `logging_cmd`: Starts or pauses sensor data logging to a specified file.
- `remove_file_cmd`: Deletes a specified file.

#### Logging Mechanism:
The `logging_cmd` function allows you to start and stop logging sensor data to a file. This integrates with the sensor library, where functions handle logging state and file selection.

### Design Task 7 (Grafana Integration & Flask Interface)
A Docker container is used to run Grafana, and a Flask HTTP server is set up to interface with Grafana. The Flask app reads sensor data from the serial interface and provides an API for Grafana to query.

The `read_serial` function reads data from the UART port, extracts JSON data, and stores it in a ring buffer. The Flask server exposes two endpoints:

- `/`: A health check endpoint that Grafana uses to verify the connection.
- `/query`: A query endpoint where Grafana can request data within a specified time range.

#### Dockerized Grafana Setup:
Grafana connects to the Flask API to query and visualize the sensor data over time with time series graphs

## Folder Structure  
```
/Users/ryan/csse4011/repo/prac1
├── README.md
├── task1
│   ├── CMakeLists.txt
|   ├── boards
│   │   └── disco_l475_iot1.overlay
│   ├── prj.conf
│   └── src
│       └── main.c
├── task7
    ├── App
    │   └── main.py
    ├──compose.yaml

```

## References  


## Build & Run Instructions for Each Task  

### **1. Set up the Zephyr environment in the `~/csse4011` folder:**  
```
source $HOME/zephyr_install/env/bin/activate
source zephyr/zephyr-env.sh
```

### **2. Navigate to the project directory:**  
```
cd ~/csse4011/repo/prac2/task1
```

### **3. Build the program for the `disco_l475_iot1` board:**  
```
west build -b disco_l475_iot1 ~/csse4011/repo/prac1/task1
```

### **4. Flash the program to the `disco_l475_iot1` board:**  
```
west flash --runner jlink
```

### **5. Run shell script to start GUI:**  
```
```
cd ~/csse4011/repo/prac2/task7/
run ./script.sh
```

west build -b esp32_devkitc/esp32/procpu --sysbuild ~/csse4011/repo/prac2/task1 --pristine

west build -b esp32_devkitc/esp32/procpu --sysbuild ~/csse4011/repo/prac2/Gateway --pristine

west build -b esp32_devkitc/esp32/procpu --sysbuild ~/csse4011/repo/prac2/Gateway --pristine

west build -b esp32_devkitc/esp32/procpu --sysbuild ~/csse4011/repo/prac2/GatewayServer --pristine

west build -b esp32_devkitc/esp32/procpu ~/csse4011/repo/prac2/SensorNode --pristine

/Users/ryan/zephyr_install/env/bin/python3 \
  /Users/ryan/csse4011/modules/hal/espressif/tools/esptool_py/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-110 --baud 115200 --no-stub write_flash -u \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  /Users/ryan/csse4011/repo/prac2/SensorNode/build/mcuboot/zephyr/zephyr.bin \
  0x20000 /Users/ryan/csse4011/repo/prac2/SensorNode/build/SensorNode/zephyr/zephyr.signed.bin

  
/Users/ryan/zephyr_install/env/bin/python3 \
  /Users/ryan/csse4011/modules/hal/espressif/tools/esptool_py/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-10 --baud 115200 --no-stub write_flash -u \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  /Users/ryan/csse4011/zephyr/samples/hello_world/build/mcuboot/zephyr/zephyr.bin \
  0x20000 /Users/ryan/csse4011/zephyr/samples/hello_world/build/hello_world/zephyr/zephyr.signed.confirmed.bin

mosquitto_pub \
  --host iot-hub-esp32-Ryan-Smith.azure-devices.net \
  --port 8883 \
  --id esp32-device-01 \
  --username "iot-hub-esp32-Ryan-Smith.azure-devices.net/esp32-device-01/?api-version=2021-04-12" \
  --pw "SharedAccessSignature sr=iot-hub-esp32-Ryan-Smith.azure-devices.net%2Fdevices%2Fesp32-device-01&sig=jwlLenolvEYvtnFtP1tnM3qVZCrQy3Pfr39sJI1iIf0%3D&se=1774248944" \
  --cafile DigiCertGlobalRootG2.pem \
  --tls-version tlsv1.2 \
  --topic "devices/esp32-device-01/messages/events/" \
  --message '{"test":"hello"}' \
  -d


  mosquitto_pub \
  --host iot-hub-esp32-Ryan-Smith.azure-devices.net \
  --port 8883 \
  --id esp32-device-01 \
  --username "iot-hub-esp32-Ryan-Smith.azure-devices.net/esp32-device-01/?api-version=2021-04-12" \
  --pw "SharedAccessSignature sr=iot-hub-esp32-Ryan-Smith.azure-devices.net%2Fdevices%2Fesp32-device-01&sig=dUYE4NNWdgzFTPBah8Z%2FssE2rk3ZYL1sSnihWJBwS%2Fw%3D&se=1774250233" \
  --cafile DigiCertGlobalRootG2.pem \
  --tls-version tlsv1.2 \
  --topic "devices/esp32-device-01/messages/events/" \
  --message '{"test":"hello"}' \
  -d


az iot hub monitor-events \
  --hub-name iot-hub-esp32-Ryan-Smith \
  --device-id esp32-device-01 \
  --output table



  /Users/ryan/zephyr_install/env/bin/python3 \
  /Users/ryan/csse4011/modules/hal/espressif/tools/esptool_py/esptool.py \
  --chip esp32 --port /dev/cu.usbserial-10 --baud 115200 --no-stub write_flash -u \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  /Users/ryan/csse4011/repo/prac2/file_mvp/build/mcuboot/zephyr/zephyr.bin \
  0x20000 /Users/ryan/csse4011/repo/prac2/file_mvp/build/file_mvp/zephyr/zephyr.signed.bin


  func azure functionapp publish iot-telemetry-func-ryansmith --python