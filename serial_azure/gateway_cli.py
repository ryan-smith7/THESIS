from serial_client import SerialClient

def set_device(serial_client: SerialClient, value: int):
    """
    Wrapper to send the 'set_device' CLI command over serial.

    Args:
        value (int): Device ID value (must be 0–255)
    """
    cmd = f"set_device {value}"
    serial_client.write(cmd)
