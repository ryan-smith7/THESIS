"""
Parse incoming byte buffers for JSON payloads embedded in log lines,
and forward valid ones via HTTP.
"""

import json
import logging
import re

from datetime import datetime

from prompt_toolkit import print_formatted_text
from prompt_toolkit.formatted_text import ANSI

from typing import Callable

logger = logging.getLogger(__name__)

def parse_buffer(buf: bytearray, received: Callable) -> bytearray:
    """
    Extract newline-terminated lines from buf, strip off any non-JSON prefix,
    attempt to parse the remainder as JSON (even if it has trailing commas),
    validate the new header/payload schema, and POST to server.

    Args:
        buf: Byte buffer that may contain partial or multiple lines.

    Returns:
        Leftover bytes after processing complete lines.
    """
    while b"\n" in buf:
        line, _, buf = buf.partition(b"\n")
        text = line.decode("utf-8", errors="ignore").strip()
        logger.debug("Raw line: %s", text)
        print_formatted_text(ANSI(text))

        # find the JSON start
        idx = text.find("{")
        if idx < 0:
            logger.debug("No JSON payload found; skipping line.")
            continue
        json_text = text[idx:]

        # remove trailing commas before } or ]
        json_text = re.sub(r",\s*}", "}", json_text)
        json_text = "".join(ch for ch in json_text if ord(ch) >= 0x20)

        try:
            msg = json.loads(json_text)
        except json.JSONDecodeError as e:
            logger.error(f"Malformed JSON; ignoring. s:{json_text}:e")
            continue

        payload = msg.get("payload", {})

        # convert ISO time → epoch
        try:
            dt = datetime.fromisoformat(payload["time"])
            payload["timestamp"] = int(dt.timestamp())
        except ValueError:
            logger.error("Invalid time format; skipping timestamp conversion.")

        received(msg)

    return buf
