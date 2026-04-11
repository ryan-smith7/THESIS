# import re
# import json
# import threading
# from collections import deque
# from datetime import datetime, timezone, timedelta
# from dateutil import parser as date_parse
# import serial
# from flask import Flask, jsonify, request

# # -------------------------------
# # Configuration and Global State
# # -------------------------------
# SERIAL_PORT = "/dev/tty.usbserial-0001"

# BAUD_RATE = 115200
# RING_BUFFER_MAXLEN = 1000

# # A thread-safe ring buffer for storing JSON messages
# ring_buffer = deque(maxlen=RING_BUFFER_MAXLEN)
# buffer_lock = threading.Lock()

# # -------------------------------
# # UART Serial Reader
# # -------------------------------
# # Function to extract the JSON content between the curly braces
# def extract_json_content(text):
#     """Extract content between the first '{' and the last '}'."""
#     match = re.search(r'\{.*\}', text)
#     if match:
#         return match.group(0)
#     return None

# def read_serial():
#     """Continuously read from the UART serial port, parse JSON lines,
#        and store valid JSON objects in the ring buffer."""
#     try:
#         ser = serial.Serial("/dev/tty.usbserial-0001", 115200, timeout=1)
#     except Exception as e:
#         print(f"Error opening serial port: {e}")
#         return

#     while True:
#         try:
#             line = ser.readline().decode('utf-8', errors='replace').strip()
#             if not line:
#                 continue
#             print(line)
#             # Extract JSON content between the curly braces
#             json_content = extract_json_content(line)

#             if json_content:
#                 # Attempt to parse the extracted JSON content
#                 try:
#                     data = json.loads(json_content)
                    
#                     # If parsing succeeds, append to the ring buffer
#                     with buffer_lock:
#                         ring_buffer.append(data)

#                 except json.JSONDecodeError:
#                     print("Not a valid JSON line:", json_content)

#         except Exception as e:
#             print("Error reading from serial port:", e)

# # -------------------------------
# # Flask HTTP Server Endpoints
# # -------------------------------
# app = Flask(__name__)

# @app.route('/', methods=['GET'])
# def health_check():
#     """Test connection endpoint for Grafana datasource configuration."""
#     return jsonify({"status": "OK"}), 200

# @app.route('/query', methods=['GET'])
# def query():

#     # Handle GET with query parameters
#     try:
#         start = date_parse.parse(request.args.get("from"))
#         end = date_parse.parse(request.args.get("to"))
#     except (ValueError, TypeError):
#         # If no dates are provided, default to last month
#         end = datetime.now(timezone.utc)
#         start = end - timedelta(days=30)

#     # Define sensor types for the dev_ids
#     sensor_types = {
#         0: "temperature",
#         1: "humidity",
#         2: "pressure",
#         3: "Gas",
#         4: ["MagX", "MagY", "MagZ"],
#         5: "light"
#     }

#     # Collect matching raw messages
#     matching_msgs = []
#     with buffer_lock:
#         for msg in list(ring_buffer):
#             dev_id = msg.get("dev_id")
#             rtc_time = msg.get("rtc_time")
#             values = msg.get("values", [])

#             if dev_id is None or rtc_time is None:
#                 continue

#             try:
#                 ts = date_parse.parse(rtc_time)
#             except Exception:
#                 continue

#             # If within the time range
#             if start <= ts <= end:
#                 if dev_id == 15:  # For dev_id 5 (ALL sensors)
#                     # Handle multiple values for dev_id 5
#                     sensor_values = [
#                         {"dev_id": 0, "rtc_time": rtc_time, "value": float(values[0]), "Sensor_type": "temperature"},
#                         {"dev_id": 1, "rtc_time": rtc_time, "value": float(values[1]), "Sensor_type": "humidity"},
#                         {"dev_id": 2, "rtc_time": rtc_time, "value": float(values[2]), "Sensor_type": "pressure"},
#                         {"dev_id": 4, "rtc_time": rtc_time, "value": float(values[3]), "Sensor_type": "MagX"},
#                         {"dev_id": 4, "rtc_time": rtc_time, "value": float(values[4]), "Sensor_type": "MagY"},
#                         {"dev_id": 4, "rtc_time": rtc_time, "value": float(values[5]), "Sensor_type": "MagZ"}
#                     ]
#                     matching_msgs.extend(sensor_values)
#                 elif dev_id in sensor_types:  # Individual sensor devices (dev_id 0, 1, 2, 4)
#                     # Handle values for individual devices
#                     if dev_id == 4:
#                         # For dev_id 4 (Magnetometer), we have multiple values
#                         sensor_values = [
#                             {"dev_id": dev_id, "rtc_time": rtc_time, "value": float(values[0]), "Sensor_type": "MagX"},
#                             {"dev_id": dev_id, "rtc_time": rtc_time, "value": float(values[1]), "Sensor_type": "MagY"},
#                             {"dev_id": dev_id, "rtc_time": rtc_time, "value": float(values[2]), "Sensor_type": "MagZ"}
#                         ]
#                         matching_msgs.extend(sensor_values)
#                     else:
#                         # For other dev_ids (0, 1, 2), we only have one value
#                         matching_msgs.append({
#                             "dev_id": dev_id,
#                             "rtc_time": rtc_time,
#                             "value": float(values[0]),
#                             "Sensor_type": sensor_types.get(dev_id)
#                         })

#     return jsonify(matching_msgs), 200
    
# # -------------------------------
# # Serial Writer
# # -------------------------------
# def write_serial(command):
#     """Write a CLI command to the UART serial port."""
#     try:
#         ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        
#         # Encode command to bytes and ensure it's properly formatted
#         command_bytes = (command + "\n").encode('utf-8')  # Append newline to indicate end of command
        
#         ser.write(command_bytes)  # Send command as bytes
#         ser.flush()  # Ensure the command is sent
#         # print(f"Sent command: {command}")
#     except Exception as e:
#         print(f"Error writing to serial port: {e}")

# def screen_loop():
#     """Interactive screen loop to display incoming data and allow command input."""
#     print("Welcome to the Serial Interface. Type 'exit' to quit.")
#     while True:
        
#         # User input for CLI commands
#         command = input("")
#         if command.lower() == "exit":
#             print("Exiting...")
#             break  # Exit the loop and the program
#         elif command == "buffer":
#             # Display the latest data from the serial port
#             with buffer_lock:
#                 if len(ring_buffer) > 0:
#                     print("Latest data in ring buffer:")
#                     for msg in list(ring_buffer):
#                         print(json.dumps(msg, indent=2))
#         elif command:
#             write_serial(command)  # Send the command to the serial port

# # -------------------------------
# # Main Execution
# # -------------------------------
# if __name__ == '__main__':

#     # Get current UTC time and format it
#     date_time = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
#     # Send it over serial
#     write_serial(f"rtc w {date_time}")
    
#     # Start the UART serial reader thread if no file is specified
#     serial_thread = threading.Thread(target=read_serial, daemon=True)
#     serial_thread.start()
#     screen_thread = threading.Thread(target=screen_loop, daemon=True)
#     screen_thread.start()

#     # Start the Flask HTTP server on all interfaces at port 5000
#     app.run(host='0.0.0.0', port=5050)
#!/usr/bin/env python3
# import re
# import json
# import threading
# from collections import deque
# from datetime import datetime, timezone, timedelta
# import serial
# from flask import Flask, jsonify, request

# # --------------------------------------------------------------------
# # Configuration
# # --------------------------------------------------------------------
# SERIAL_PORT = "/dev/tty.usbserial-0001"
# BAUD_RATE = 115200
# RING_BUFFER_MAXLEN = 5000

# ring_buffer = deque(maxlen=RING_BUFFER_MAXLEN)
# buffer_lock = threading.Lock()

# # --------------------------------------------------------------------
# # Helpers
# # --------------------------------------------------------------------
# def extract_json_objects(buffer):
#     """Yield all complete JSON objects from a growing text buffer."""
#     objs = []
#     depth = 0
#     start = None
#     for i, ch in enumerate(buffer):
#         if ch == "{":
#             if depth == 0:
#                 start = i
#             depth += 1
#         elif ch == "}":
#             depth -= 1
#             if depth == 0 and start is not None:
#                 objs.append(buffer[start:i + 1])
#                 start = None
#     return objs


# def parse_timestamp(rtc_time_str):
#     """Convert Zephyr uptime (ms) to real timestamp."""
#     try:
#         ms = int(rtc_time_str)
#         # use current wall time as base, minus uptime remainder
#         now = datetime.now(timezone.utc)
#         t = now - timedelta(milliseconds=(ms % (24 * 3600 * 1000)))
#         return int(t.timestamp() * 1000)
#     except Exception:
#         return int(datetime.now(timezone.utc).timestamp() * 1000)

# # --------------------------------------------------------------------
# # UART Reader Thread
# # --------------------------------------------------------------------
# def read_serial():
#     """Continuously read serial lines, robustly parse JSON objects."""
#     try:
#         ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
#         print(f"[INFO] Connected to {SERIAL_PORT} at {BAUD_RATE} baud.")
#     except Exception as e:
#         print(f"[ERROR] Cannot open serial port: {e}")
#         return

#     text_buffer = ""
#     while True:
#         try:
#             data = ser.read(ser.in_waiting or 1).decode("utf-8", errors="ignore")
#             if not data:
#                 continue
#             text_buffer += data

#             # Show raw characters as they arrive
#             if "\n" in data:
#                 for line in data.splitlines():
#                     if line.strip():
#                         print(f"[RAW] {line.strip()}")

#             # Find and decode complete JSON objects
#             for obj_text in extract_json_objects(text_buffer):
#                 try:
#                     msg = json.loads(obj_text)
#                     with buffer_lock:
#                         ring_buffer.append(msg)
#                     print(f"[OK] dev_id={msg.get('dev_id')} -> {msg.get('values')}")
#                 except json.JSONDecodeError:
#                     print(f"[WARN] Invalid JSON: {obj_text[:100]}...")
#                 text_buffer = text_buffer[text_buffer.find(obj_text) + len(obj_text):]

#         except Exception as e:
#             print(f"[ERROR] Serial read error: {e}")
#             break

# # --------------------------------------------------------------------
# # Flask Server
# # --------------------------------------------------------------------
# app = Flask(__name__)

# @app.route("/", methods=["GET"])
# def health():
#     return jsonify({"status": "OK"})

# @app.route("/search", methods=["POST"])
# def grafana_search():
#     """Return list of available metric names."""
#     metrics = [
#         "BME280_Temperature", "BME280_Pressure", "BME280_Humidity",
#         "ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"
#     ]
#     for wl in [405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855]:
#         metrics.append(f"AS7343_{wl}nm")
#     return jsonify(metrics)

# @app.route("/annotations", methods=["POST"])
# def grafana_annotations():
#     return jsonify([])

# @app.route("/query", methods=["POST"])
# def grafana_query():
#     """Main Grafana datasource query handler."""
#     try:
#         payload = request.get_json(force=True)
#     except Exception as e:
#         return jsonify({"error": f"Invalid JSON: {e}"}), 400

#     try:
#         start = datetime.fromisoformat(payload["range"]["from"].replace("Z", "+00:00"))
#         end = datetime.fromisoformat(payload["range"]["to"].replace("Z", "+00:00"))
#     except Exception:
#         end = datetime.now(timezone.utc)
#         start = end - timedelta(hours=1)

#     series = {}

#     with buffer_lock:
#         for msg in list(ring_buffer):
#             dev_id = msg.get("dev_id")
#             rtc_time = str(msg.get("rtc_time"))
#             values = msg.get("values", [])
#             if dev_id is None or not values:
#                 continue

#             ts_ms = parse_timestamp(rtc_time)
#             t_dt = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc)
#             if not (start <= t_dt <= end):
#                 continue

#             # --------------------------------------------------------
#             # BME280 (dev_id = 1): temp, press, humid
#             # --------------------------------------------------------
#             if dev_id == 1 and len(values) >= 3:
#                 names = ["BME280_Temperature", "BME280_Pressure", "BME280_Humidity"]
#                 for i, name in enumerate(names):
#                     try:
#                         val = float(values[i])
#                         series.setdefault(name, []).append([val, ts_ms])
#                     except Exception:
#                         continue

#             # --------------------------------------------------------
#             # ENS160 (dev_id = 2): eCO2, TVOC, AQI
#             # --------------------------------------------------------
#             elif dev_id == 2 and len(values) >= 3:
#                 names = ["ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"]
#                 for i, name in enumerate(names):
#                     try:
#                         val = float(values[i])
#                         series.setdefault(name, []).append([val, ts_ms])
#                     except Exception:
#                         continue

#             # --------------------------------------------------------
#             # AS7343 (dev_id = 3): spectral bands λ=...
#             # --------------------------------------------------------
#             elif dev_id == 3:
#                 for entry in values:
#                     m = re.match(r"λ=(\d+)nm:(\d+)", entry)
#                     if m:
#                         wl = int(m.group(1))
#                         val = float(m.group(2))
#                         name = f"AS7343_{wl}nm"
#                         series.setdefault(name, []).append([val, ts_ms])

#     resp = [{"target": name, "datapoints": pts} for name, pts in series.items() if pts]
#     return jsonify(resp), 200

# @app.route("/latest", methods=["GET"])
# def show_latest():
#     """Display recent JSON messages in browser for debugging."""
#     with buffer_lock:
#         # Show last 20 messages, newest last
#         recent = list(ring_buffer)[-20:]
#     return jsonify(recent)

# # --------------------------------------------------------------------
# # Main Entrypoint
# # --------------------------------------------------------------------
# if __name__ == "__main__":
#     serial_thread = threading.Thread(target=read_serial, daemon=True)
#     serial_thread.start()
#     print("[INFO] Grafana bridge running on http://0.0.0.0:5050")
#     app.run(host="0.0.0.0", port=5050)

# import re
# import json
# import threading
# from collections import deque
# from datetime import datetime, timezone, timedelta
# import serial
# from flask import Flask, jsonify, request

# # -------------------------------
# # Configuration
# # -------------------------------
# SERIAL_PORT = "/dev/tty.usbserial-0001"
# BAUD_RATE = 115200
# RING_BUFFER_MAXLEN = 2000

# # Thread-safe ring buffer
# ring_buffer = deque(maxlen=RING_BUFFER_MAXLEN)
# buffer_lock = threading.Lock()

# # -------------------------------
# # UART Serial Reader
# # -------------------------------
# def extract_json_content(text):
#     """Extract JSON object between first '{' and last '}'."""
#     match = re.search(r"\{.*\}", text)
#     return match.group(0) if match else None

# def read_serial():
#     """Continuously read serial, parse JSON, print, and store."""
#     try:
#         ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
#         print(f"[INFO] Connected to {SERIAL_PORT} at {BAUD_RATE} baud.")
#     except Exception as e:
#         print(f"[ERROR] Cannot open serial port: {e}")
#         return

#     while True:
#         try:
#             line = ser.readline().decode("utf-8", errors="ignore").strip()
#             if not line:
#                 continue
#             print(line)  # Echo to terminal for debug visibility
#             json_str = extract_json_content(line)
#             if not json_str:
#                 continue

#             try:
#                 msg = json.loads(json_str)
#                 with buffer_lock:
#                     ring_buffer.append(msg)
#             except json.JSONDecodeError:
#                 print(f"[WARN] Invalid JSON: {json_str[:80]}...")

#         except Exception as e:
#             print(f"[ERROR] Serial read error: {e}")

# # -------------------------------
# # Flask HTTP Server
# # -------------------------------
# app = Flask(__name__)

# @app.route("/", methods=["GET"])
# def health():
#     return jsonify({"status": "OK"}), 200


# @app.route("/query", methods=["GET"])
# def query():
#     """Grafana-compatible query endpoint."""
#     try:
#         start_str = request.args.get("from")
#         end_str = request.args.get("to")
#         start = datetime.fromisoformat(start_str.replace("Z", "+00:00")) if start_str else datetime.now(timezone.utc) - timedelta(hours=1)
#         end = datetime.fromisoformat(end_str.replace("Z", "+00:00")) if end_str else datetime.now(timezone.utc)
#     except Exception:
#         start = datetime.now(timezone.utc) - timedelta(hours=1)
#         end = datetime.now(timezone.utc)

#     result = []
#     with buffer_lock:
#         for msg in list(ring_buffer):
#             dev_id = msg.get("dev_id")
#             rtc_time = msg.get("rtc_time")
#             values = msg.get("values", [])
#             if dev_id is None or not values:
#                 continue

#             # convert rtc_time (ms uptime) to timestamp
#             try:
#                 ts_ms = int(rtc_time)
#             except Exception:
#                 ts_ms = int(datetime.now(timezone.utc).timestamp() * 1000)

#             # Device-specific unpacking
#             if dev_id == 1:  # BME280
#                 names = ["BME280_Temperature", "BME280_Pressure", "BME280_Humidity"]
#                 for i, name in enumerate(names[:len(values)]):
#                     try:
#                         result.append({"target": name, "datapoints": [[float(values[i]), ts_ms]]})
#                     except Exception:
#                         continue

#             elif dev_id == 2:  # ENS160
#                 names = ["ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"]
#                 for i, name in enumerate(names[:len(values)]):
#                     try:
#                         result.append({"target": name, "datapoints": [[float(values[i]), ts_ms]]})
#                     except Exception:
#                         continue

#             elif dev_id == 3:  # AS7343
#                 for v in values:
#                     match = re.match(r"λ=(\d+)nm:(\d+)", v)
#                     if match:
#                         wl = match.group(1)
#                         val = float(match.group(2))
#                         result.append({"target": f"AS7343_{wl}nm", "datapoints": [[val, ts_ms]]})

#     return jsonify(result), 200


# @app.route("/search", methods=["GET", "POST"])
# def search():
#     """Return available metrics for Grafana panel autocomplete."""
#     metrics = [
#         "BME280_Temperature", "BME280_Pressure", "BME280_Humidity",
#         "ENS160_eCO2", "ENS160_TVOC", "ENS160_AQI"
#     ]
#     for wl in [405, 425, 450, 475, 515, 550, 555, 600, 640, 690, 745, 855]:
#         metrics.append(f"AS7343_{wl}nm")
#     return jsonify(metrics)


# @app.route("/annotations", methods=["POST"])
# def annotations():
#     """Empty annotations endpoint for Grafana."""
#     return jsonify([])


# # -------------------------------
# # Serial Writer / Interactive Shell
# # -------------------------------
# def write_serial(command):
#     try:
#         with serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1) as ser:
#             ser.write((command + "\n").encode("utf-8"))
#             ser.flush()
#     except Exception as e:
#         print(f"[ERROR] Serial write failed: {e}")


# def screen_loop():
#     print("Type 'exit' to quit or 'buffer' to view latest JSONs.\n")
#     while True:
#         cmd = input("> ")
#         if cmd.lower() == "exit":
#             print("Exiting...")
#             break
#         elif cmd.lower() == "buffer":
#             with buffer_lock:
#                 for msg in list(ring_buffer)[-10:]:
#                     print(json.dumps(msg, indent=2))
#         elif cmd:
#             write_serial(cmd)


# # -------------------------------
# # Main
# # -------------------------------
# if __name__ == "__main__":
#     serial_thread = threading.Thread(target=read_serial, daemon=True)
#     serial_thread.start()

#     screen_thread = threading.Thread(target=screen_loop, daemon=True)
#     screen_thread.start()

#     print("[INFO] Grafana bridge running at http://0.0.0.0:5050")
#     app.run(host="0.0.0.0", port=5050)

