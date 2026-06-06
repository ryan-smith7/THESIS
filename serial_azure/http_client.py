"""
Simple HTTP client for posting JSON payloads.
"""

import logging
import requests
import os
import json

from uuid import uuid4
from datetime import datetime

logger = logging.getLogger(__name__)

from dotenv import load_dotenv

load_dotenv()

def send_test_message_to_local_server():
    """
    POST a JSON payload to the configured server.
    """

    payload = {
      "header": {
        "messageId": f"{uuid4()}",
        "gatewayId": "GW-01",
        "timestamp": f"{datetime.now().isoformat()}Z",
        "schemaVersion": "1.0",
        "messageType": "telemetry",
      },
      "payload": {
        "messageId": f"{uuid4()}",
        "deviceId": "dev-1",
        "timestamp": f"{datetime.now().isoformat()}Z",
        "uptime": "494",
        "location": {
          "latitude": "27.5002432",
          "ns": "S",
          "longitude": "153.0153600",
          "ew": "E",
          "altitude_m": "0.0"
        },
        "environment": {
          "temperature_c": "26.22",
          "humidity_percent": "69.00",
          "pressure_hpa": "102.3",
          "gas_ppm": "28.00"
        },
        "acceleration": {
          "x_mps2": "2.413",
          "y_mps2": "-0.459",
          "z_mps2": "-6.511"
        },
        "hash": "FBDA00C64513C32B027DA202C4C0574E86CE9FE462C42B8BB3B7734380810DA8",
      },
      "signature": {
        "alg": "HS256",
        "keyId": "key-001",
        "value": "FBDA00C64513C32B027DA202C4C0574E86CE9FE462C42B8BB3B7734380810DA8"
      }
    }

    server_url = os.getenv("LOCAL_SERVER_URL", "http://localhost:8000/api/telemetry/gateway/")

    headers = {
        "Authorization": f"Api-Key {os.getenv('API_KEY')}",
        "Content-Type": "application/json",
    }

    try:
        resp = requests.post(server_url, headers=headers, data=json.dumps(payload), timeout=5)
        resp.raise_for_status()

        logger.info("Posted JSON → %s [%d]", server_url, resp.status_code)

    except Exception as exc:
        logger.error("Failed to POST JSON: %s %s", exc, resp.text if 'resp' in locals() else "")

