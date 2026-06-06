#!/usr/bin/env python3
"""
sntp_oracle.py — Shared SNTP-over-HTTP client for the Oracle time server.

Implements the same 4-timestamp exchange as http_time_sync.c on the gateway:
  t1 — local time() at request send
  t2 — server UTC at request arrival   (from binary body)
  t3 — server UTC at response send     (from binary body)
  t4 — local time() at response receipt

  θ = ((t2 - t1) + (t3 - t4)) / 2
  corrected_utc = local_time() + θ

Returns corrected UTC as a float (seconds.milliseconds) and the RTT.
Import this module from timeDrift_gateway.py and timeDrift_node.py.

Also runnable standalone for a quick one-shot check:
  python3 sntp_oracle.py
"""

import socket
import struct
import time

TIME_SERVER_HOST = "161.33.232.177"
TIME_SERVER_PORT = 80
TIMEOUT_S        = 5
HTTP_REQUEST     = (
    b"GET /time HTTP/1.0\r\n"
    b"Host: 161.33.232.177\r\n"
    b"\r\n"
)

def sntp_query():
    """
    Performs one SNTP-over-HTTP exchange with the Oracle time server.

    Returns:
        (corrected_utc_float, rtt_ms, theta_ms)
          corrected_utc_float — corrected UTC as seconds.fraction (float)
          rtt_ms              — round-trip time in milliseconds
          theta_ms            — computed clock offset θ in milliseconds

    Raises:
        OSError / struct.error on network or parse failure.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT_S)
    sock.connect((TIME_SERVER_HOST, TIME_SERVER_PORT))

    t1 = time.time()
    sock.sendall(HTTP_REQUEST)

    # Read until server closes (HTTP/1.0)
    buf = b""
    try:
        while True:
            chunk = sock.recv(512)
            if not chunk:
                break
            buf += chunk
    finally:
        sock.close()

    t4 = time.time()

    # Split headers from body
    sep = buf.find(b"\r\n\r\n")
    if sep < 0:
        raise ValueError("No HTTP header separator found")
    body = buf[sep + 4:]

    if len(body) < 12:
        raise ValueError(f"Body too short: {len(body)} bytes")

    # Unpack 12-byte binary body — same layout as C client
    t2_sec = struct.unpack_from(">I", body, 0)[0]
    t2_ms  = struct.unpack_from(">H", body, 4)[0]
    t3_sec = struct.unpack_from(">I", body, 6)[0]
    t3_ms  = struct.unpack_from(">H", body, 10)[0]

    t2 = t2_sec + t2_ms / 1000.0
    t3 = t3_sec + t3_ms / 1000.0

    # SNTP offset θ = ((t2 - t1) + (t3 - t4)) / 2
    theta    = ((t2 - t1) + (t3 - t4)) / 2.0
    rtt_ms   = int((t4 - t1) * 1000)
    theta_ms = theta * 1000.0

    corrected_utc = t4 + theta

    return corrected_utc, rtt_ms, theta_ms


def get_corrected_utc():
    """
    Convenience wrapper — returns just the corrected UTC float.
    Retries once on failure.
    """
    try:
        utc, _, _ = sntp_query()
        return utc
    except Exception:
        time.sleep(0.5)
        utc, _, _ = sntp_query()
        return utc


if __name__ == "__main__":
    import datetime
    print(f"Querying Oracle time server at {TIME_SERVER_HOST}:{TIME_SERVER_PORT}...")
    utc, rtt_ms, theta_ms = sntp_query()
    pc_now = datetime.datetime.now(datetime.timezone.utc)
    pc_utc = pc_now.timestamp()

    sec  = int(utc)
    ms   = int((utc - sec) * 1000)
    dt   = datetime.datetime.fromtimestamp(utc, tz=datetime.timezone.utc)

    print(f"\n  Oracle SNTP UTC : {sec}.{ms:03d}  ({dt.strftime('%Y-%m-%d %H:%M:%S.') + f'{ms:03d}'})")
    print(f"  PC NTP UTC      : {pc_now.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
    print(f"  RTT             : {rtt_ms} ms")
    print(f"  θ (offset)      : {theta_ms:+.1f} ms")
    print(f"  Oracle vs PC    : {(utc - pc_utc)*1000:+.1f} ms")
    print()
    print("  If Oracle vs PC ≈ 0 ms  → PC NTP and Oracle agree, gateway SNTP is working.")
    print("  If Oracle vs PC ≈ -960 ms → Oracle server itself is ~960 ms behind NTP.")
