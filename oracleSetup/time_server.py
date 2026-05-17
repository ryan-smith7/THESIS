#!/usr/bin/env python3
"""
Time server for SNTP-over-HTTP synchronisation.

Returns a 12-byte binary body containing four big-endian fields:
  t2_sec  uint32  — UTC seconds   when request arrived
  t2_ms   uint16  — UTC ms        when request arrived
  t3_sec  uint32  — UTC seconds   when response was sent
  t3_ms   uint16  — UTC ms        when response was sent

The ESP32 client uses these four timestamps to compute the SNTP clock
offset θ = ((t2 − t1) + (t3 − t4)) / 2, where t1/t4 are its own
uptime readings bracketing the round-trip.
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
import struct
import time

arrival_times = {}

class TimeHandler(BaseHTTPRequestHandler):
    timeout = 5

    def do_GET(self):
        t2 = arrival_times.pop(self.request.fileno(), time.time())

        if self.path == '/health':
            t = time.time()
            sec = int(t)
            ms  = int((t - sec) * 1000)
            body = f"OK {sec}.{ms:03d}\n".encode()

            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', len(body))
            self.end_headers()
            self.wfile.write(body)
            return

        def split(t):
            sec = int(t)
            ms  = int((t - sec) * 1000)
            return sec, ms

        t2_sec, t2_ms = split(t2)

        t3 = time.time()
        t3_sec, t3_ms = split(t3)

        body = struct.pack('>IHIH', t2_sec, t2_ms, t3_sec, t3_ms)

        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads      = True
    allow_reuse_address = True

    def get_request(self):
        request, client_address = self.socket.accept()
        arrival_times[request.fileno()] = time.time()
        return request, client_address


ThreadedHTTPServer(('0.0.0.0', 80), TimeHandler).serve_forever()