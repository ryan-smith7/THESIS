import os
import json
import logging
import uuid
import hmac
import hashlib
import base64
import time
from urllib.parse import quote, urlencode
from datetime import datetime

import requests
from azure.iot.device import IoTHubDeviceClient, Message
from typing import Callable, Dict, Any, Optional
from dotenv import load_dotenv

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

load_dotenv()


class AzureIoTHubMqttClient:
    """
    Wrapper around Azure IoT Hub DeviceClient (MQTT) to send/receive JSON telemetry.
    """

    def __init__(self, message_callback: Optional[Callable] = None, connection_string: Optional[str] = None) -> None:
        conn_str = connection_string or os.getenv("IOTHUB_DEVICE_CONNECTION_STRING")
        if not conn_str:
            raise ValueError(
                "IoT Hub device connection string must be provided either "
                "via constructor or IOTHUB_DEVICE_CONNECTION_STRING env var"
            )

        if message_callback:
            self.message_callback = message_callback

        self.client = IoTHubDeviceClient.create_from_connection_string(conn_str)
        self.client.on_message_received = self.receive_telemetry
        logger.info("Azure IoT Hub client initialized.")

    def send_telemetry(self, data: Dict[str, Any]) -> None:
        payload = json.dumps(data)
        msg = Message(payload)
        msg.content_encoding = "utf-8"
        msg.content_type = "application/json"
        logger.debug("Sending telemetry: %s", payload)
        self.client.send_message(msg)
        logger.info("Telemetry successfully sent.")

    def receive_telemetry(self, message: bytes) -> None:
        logger.info("IoT received: %r", message)
        try:
            raw_bytes = message.data if hasattr(message, "data") else message
            raw_str = raw_bytes.decode("utf-8") if isinstance(raw_bytes, (bytes, bytearray)) else str(raw_bytes)
        except Exception as e:
            logger.error("Failed to extract payload: %s", e, exc_info=True)
            return

        try:
            message_parsed = json.loads(raw_str)
            logger.info("Parsed JSON: %s", message_parsed)
        except json.JSONDecodeError as e:
            logger.error("JSON decode error: %s -- payload: %s", e, raw_str)
            return

        try:
            self.message_callback(message_parsed)
        except Exception as e:
            logger.error("message_callback raised: %s", e, exc_info=True)

    def shutdown(self) -> None:
        self.client.shutdown()
        logger.info("IoT Hub client shut down.")


def _generate_sas_token(connection_string: str, device_id: str, expiry_seconds: int = 3600) -> str:
    """
    Generate a SAS token from a hub-level connection string.
    Used for the REST API cloud-to-device call.
    """
    # Parse connection string
    parts = dict(part.split("=", 1) for part in connection_string.split(";"))
    hostname = parts["HostName"]
    key_name = parts["SharedAccessKeyName"]
    key = parts["SharedAccessKey"]

    uri = quote(f"{hostname}/devices/{device_id}", safe="")
    expiry = int(time.time()) + expiry_seconds
    to_sign = f"{uri}\n{expiry}".encode("utf-8")

    signature = base64.b64encode(
        hmac.new(base64.b64decode(key), to_sign, hashlib.sha256).digest()
    ).decode("utf-8")

    token = (
        f"SharedAccessSignature sr={uri}"
        f"&sig={quote(signature, safe='')}"
        f"&se={expiry}"
        f"&skn={key_name}"
    )
    return token


def send_json_to_azure_iot_hub(data: Dict[str, Any]) -> None:
    """Convenience function — send a single telemetry message then shut down."""
    client = AzureIoTHubMqttClient()
    try:
        client.send_telemetry(data)
    finally:
        client.shutdown()


def send_test_message_to_azure_iot_hub() -> None:
    """Send a hardcoded test telemetry payload to verify device → cloud works."""
    payload = {
        "header": {
            "messageId": f"{uuid.uuid4()}",
            "gatewayId": "GW-01",
            "schemaVersion": "1.0",
            "messageType": "telemetry"
        },
        "payload": {
            "deviceId": "dev-1",
            "timestamp": datetime.now().isoformat(),
            "uptime": "494",
            "location": {
                "latitude": "27.5002432", "ns": "S",
                "longitude": "153.0153600", "ew": "E",
                "altitude_m": "0.0"
            },
            "environment": {
                "temperature_c": "26.22", "humidity_percent": "69.00",
                "pressure_hpa": "102.3", "gas_ppm": "28.00"
            },
            "acceleration": {
                "x_mps2": "2.413", "y_mps2": "-0.459", "z_mps2": "-6.511"
            }
        },
        "signature": {
            "alg": "HS256",
            "keyId": "key-001",
            "value": "FBDA00C64513C32B027DA202C4C0574E86CE9FE462C42B8BB3B7734380810DA8"
        }
    }
    send_json_to_azure_iot_hub(payload)
    logger.info("Test message sent to Azure IoT Hub.")


def send_iot_hub_test_message(connection_string: Optional[str] = None) -> None:
    """
    Send a cloud-to-device message using the IoT Hub REST API directly.
    Replaces IoTHubRegistryManager to avoid the uamqp dependency.
    """
    conn_str = connection_string or os.getenv("IOTHUB_CONNECTION_STRING")
    if not conn_str:
        raise ValueError("IOTHUB_CONNECTION_STRING not set")

    device_id = "esp32-device-01"

    # Parse hostname from connection string
    parts = dict(part.split("=", 1) for part in conn_str.split(";"))
    hostname = parts["HostName"]

    # Generate SAS token
    sas_token = _generate_sas_token(conn_str, device_id)

    # Build REST API URL for cloud-to-device message
    url = f"https://{hostname}/devices/{device_id}/messages/devicebound?api-version=2021-04-12"

    message = {
        "messageType": "deviceIDUpdate",
        "message": 10
    }

    headers = {
        "Authorization": sas_token,
        "Content-Type": "application/json",
    }

    response = requests.post(url, headers=headers, data=json.dumps(message))

    if response.status_code in (200, 204):
        logger.info("Cloud-to-device message sent successfully via REST API.")
    else:
        logger.error("Failed to send C2D message: %s %s", response.status_code, response.text)
        raise RuntimeError(f"C2D REST call failed: {response.status_code}")