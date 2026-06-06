"""
Entry point for the serial-terminal CLI.

Uses argparse to parse CLI options, starts the
SerialClient, and reads user input lines to send.

Example

python3 main.py --port /dev/ttyACM0 --baud 115200
"""
from message_hash import compute_tracker_hash

import argparse
import logging
from serial_client import SerialClient
from message_parser import parse_buffer
from prompt_toolkit import PromptSession
from prompt_toolkit.patch_stdout import patch_stdout
from azure_iothub import AzureIoTHubMqttClient, send_test_message_to_azure_iot_hub, send_iot_hub_test_message
from http_client import send_test_message_to_local_server
from gateway_cli import set_device

serial_client_global = None

# Configure root logger
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)


def main():
    """
    Parse arguments, start serial client, and handle user I/O loop.
    """
    parser = argparse.ArgumentParser(
        description="Serial terminal that forwards JSON messages via HTTP"
    )
    parser.add_argument(
        "-p", "--port",
        default="/dev/ttyACM0",
        help="Serial port device"
    )
    parser.add_argument(
        "-b", "--baud",
        type=int,
        default=115200,
        help="Baud rate"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Enable debug logging"
    )
    parser.add_argument(
        "--az-test",
        action="store_true",
        help="Run a test to send a message to Azure IoT Hub"
    )
    parser.add_argument(
        "--az-mqtt",
        action="store_true",
        help="Run a test to send a message to Azure IoT Hub"
    )
    parser.add_argument(
        "--local-test",
        action="store_true",
        help="Run a test to send a message to the local HTTP server"
    )

    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    if args.az_test:
        send_test_message_to_azure_iot_hub()
        return

    if args.az_mqtt:
        send_iot_hub_test_message()
        return

    if args.local_test:
        send_test_message_to_local_server()
        return
 
    azure_client = AzureIoTHubMqttClient(message_callback=process_messages)

    serial_client = SerialClient(args.port, args.baud, parser_callback=parse_buffer, received_callback=azure_client.send_telemetry)
    serial_client.start()

    global serial_client_global
    serial_client_global = serial_client

    print(f"Connected to {args.port} @ {args.baud}. Type 'exit' to quit.")

    session = PromptSession("> ")

    # patch_stdout lets us safely print() from other threads without breaking the prompt.
    with patch_stdout():
        try:
            while True:
                line = session.prompt()
                if line.strip().lower() == "exit":
                    break
                serial_client.write(line)
        except (KeyboardInterrupt, EOFError):
            print("\nExiting…")
        finally:
            serial_client.stop()
            azure_client.shutdown()

def process_messages(message: dict):
    """
        Check messages for 'deviceIDupdate' and call set_device() if found.
    """
    if message["messageType"] == "deviceIDUpdate":
        global serial_client_global

        if not serial_client_global:
            return

        value = message["message"]

        print(f"deviceIDupdate message received with value: {value}")

        set_device(serial_client_global, value)

if __name__ == "__main__":
    main()
    # full_msg = '''
    # {
    #   "header": { },
    #     "payload":{"timestamp":"2025-05-29T13:27:32","uptime":"61","location":{"latitude":"27.5001869","ns":"S","longitude":"153.0141296","ew":"E","altitude_m":"31.0"},"environment":{"temperature_c":"25.55","humidity_percent":"58.50","pressure_hpa":"102.1","gas_ppm":"14.00"},"acceleration":{"x_mps2":"0.038","y_mps2":"-0.268","z_mps2":"-9.690"}},
    #   "signature": { }
    # }
    # '''
    # print("Tracker hash =", compute_tracker_hash(full_msg))
