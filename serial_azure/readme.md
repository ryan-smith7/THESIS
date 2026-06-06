# Serial CLI JSON Forwarder

A Python command-line interface (CLI) tool that emulates a serial terminal, allowing you to send and receive data over a serial port. Incoming JSON messages matching a specific schema are automatically forwarded via HTTP to a configured server.

## Features

* **Interactive serial terminal**: Send and receive data on a serial port.
* **Configurable connection**: Specify port, baud rate, and logging verbosity via command-line arguments.
* **JSON parsing**: Detects newline-terminated JSON messages matching the schema:

  ```json
  {"type": <int>, "timestamp": <int>, "message": { ... }}
  ```
* **HTTP forwarding**: Automatically POSTs valid JSON messages to a server URL defined in a configuration file.
* **Threaded I/O**: Non-blocking serial read loop in a background thread.

## Project Structure

```
my_serial_cli/
├── config.ini          # INI file for server URL
├── requirements.txt    # Python dependencies
├── README.md           # This README
├── settings.py         # Loads config values
├── serial_client.py    # Serial I/O client (threaded)
├── message_parser.py   # Parses buffer and forwards JSON
├── http_client.py      # HTTP POST helper
└── main.py             # CLI entry point (argparse)
```

## Prerequisites

* Python 3.7+
* pip (for installing dependencies)
* A serial device (e.g., USB-to-UART adapter)
* Access to the target HTTP server endpoint

## Installation

1. (Optional) Create and activate a virtual environment:

   ```bash
   python3 -m venv venv
   source venv/bin/activate
   ```

2. Install Python dependencies:

   ```bash
   pip install -r requirements.txt
   ```

## Configuration

Edit the `config.ini` file to set your server URL:

```ini
[server]
url = http://your-server.example.com/api/sensor
```

## Usage

Run the CLI with the desired serial port and baud rate:

```bash
python main.py --port /dev/ttyACM0 --baud 115200
```

* **--port / -p**: Serial port device (default: `/dev/ttyACM0`)
* **--baud / -b**: Baud rate (default: `115200`)
* **--verbose / -v**: Enable debug logging

Once running, type any text and press **Enter** to send it over serial. Incoming lines that parse as valid JSON matching the schema will be POSTed to your configured server.

To exit, type `exit` or press <kbd>Ctrl+C</kbd>.

## Examples

Start the client listening on `/dev/ttyACM0` at 115200 baud with verbose logging:

```bash
python main.py -p /dev/ttyACM0 -b 115200 -v
```

Sample incoming JSON that will be forwarded:

```json
{"type": 1, "timestamp": 1746137544, "message": {"x": 10, "y": 20}}
```


<!-- -- Drop and recreate telemetry table with clean schema
DROP TABLE IF EXISTS telemetry;

CREATE TABLE telemetry (
    id                INT             IDENTITY(1,1) PRIMARY KEY,
    received_at       DATETIME2       DEFAULT GETUTCDATE(),

    -- Identity
    device_id         NVARCHAR(32)    NOT NULL,
    gateway_id        NVARCHAR(32)    NOT NULL,
    message_id        NVARCHAR(64)    NOT NULL,

    -- Timing
    timestamp         DATETIME2       NULL,
    uptime_ms         BIGINT          DEFAULT 0,
    proto_ver         TINYINT         DEFAULT 0,

    -- Environment
    temperature_c     FLOAT           DEFAULT 0.0,
    humidity_percent  FLOAT           DEFAULT 0.0,
    pressure_hpa      FLOAT           DEFAULT 0.0,
    eco2_ppm          INT             DEFAULT 0,
    tvoc_ppb          INT             DEFAULT 0,
    aqi               TINYINT         DEFAULT 0,
    batt_mv           INT             DEFAULT 0,

    -- Spectrum (AS7343)
    as7343_405nm      INT             DEFAULT 0,
    as7343_425nm      INT             DEFAULT 0,
    as7343_450nm      INT             DEFAULT 0,
    as7343_475nm      INT             DEFAULT 0,
    as7343_515nm      INT             DEFAULT 0,
    as7343_550nm      INT             DEFAULT 0,
    as7343_555nm      INT             DEFAULT 0,
    as7343_600nm      INT             DEFAULT 0,
    as7343_640nm      INT             DEFAULT 0,
    as7343_690nm      INT             DEFAULT 0,
    as7343_745nm      INT             DEFAULT 0,
    as7343_855nm      INT             DEFAULT 0,
    as7343_visible    INT             DEFAULT 0,

    -- Raw
    raw_payload       NVARCHAR(MAX)   NULL
); -->