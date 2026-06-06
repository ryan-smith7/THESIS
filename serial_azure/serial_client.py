"""
SerialClient: threaded serial port reader/writer.

Reads from a serial port in a background thread and
passes raw bytes to a parser callback, and provides
a simple write() method for sending lines.
"""

import threading
import logging
import serial

logger = logging.getLogger(__name__)


class SerialClient:
    """
    Threaded client for non-blocking serial I/O.

    Attributes:
        ser: The underlying Serial object.
        parser_callback: Function that consumes a bytearray
            and returns any leftover bytes.
    """

    def __init__(self, port: str, baudrate: int, parser_callback, received_callback):
        """
        Initialize SerialClient.

        Args:
            port: Serial port path (e.g. "/dev/ttyUSB0").
            baudrate: Baud rate for communication.
            parser_callback: Callable[[bytearray], bytearray]
                to handle incoming data.
        """
        try:
            self.ser = serial.Serial(port, baudrate, timeout=0.1)
        except Exception as e:
            logger.error(f"Error initialising serial port failed with {e}")
            exit(1)

        self.received_callback = received_callback
        self.parser_callback = parser_callback
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._read_loop, daemon=True
        )

    def start(self):
        """Start the background reader thread."""
        self._thread.start()
        logger.info("Started reader on %s @ %d", self.ser.port, self.ser.baudrate)

    def stop(self):
        """Signal the thread to stop and close the serial port."""
        self._stop_event.set()
        self._thread.join()
        self.ser.close()
        logger.info("Serial client stopped.")

    def write(self, data: str):
        """
        Send a line over serial (appends newline).

        Args:
            data: The text line to send.
        """
        line = data + "\n"
        self.ser.write(line.encode("utf-8"))
        logger.debug("Sent: %s", line.strip())

    def _read_loop(self):
        """
        Continuously read from serial until stopped,
        and feed data to parser_callback.
        """
        buffer = bytearray()
        while not self._stop_event.is_set():
            chunk = self.ser.read(1024)
            if chunk:
                buffer.extend(chunk)
                buffer = self.parser_callback(buffer, self.received_callback)
