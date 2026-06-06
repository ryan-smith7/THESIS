import json
import hashlib

JSON_BUFFER_SIZE = 352

def compute_tracker_hash(json_message: str) -> str:
    # 1. parse the full JSON, dig into payload
    msg = json.loads(json_message)
    payload = msg['payload']

    # 2. pull out the *string* values exactly as C printed them:
    ts = payload['timestamp'] # e.g. "2025-05-29T13:27:32"
    up = payload['uptime'] # e.g. "61"
    lat = payload['location']['latitude'] # e.g. "27.5001869"
    ns = payload['location']['ns'] # "S"
    lon = payload['location']['longitude'] # e.g. "153.0141296"
    ew = payload['location']['ew'] # "E"
    alt = payload['location']['altitude_m'] # e.g. "31.0"
    temp = payload['environment']['temperature_c'] # e.g. "25.55"
    hum = payload['environment']['humidity_percent'] # e.g. "58.50"
    pres = payload['environment']['pressure_hpa'] # e.g. "102.1"
    gas = payload['environment']['gas_ppm'] # e.g. "14.00"
    x = payload['acceleration']['x_mps2']  # e.g. "0.038"
    y = payload['acceleration']['y_mps2']  # e.g. "-0.268"
    z = payload['acceleration']['z_mps2']  # e.g. "-9.690"

    # 3. rebuild the exact same snippet that C’s snprintf wrote:
    snippet = (
        f'"payload":{{'
          f'"timestamp":"{ts}",'
          f'"uptime":"{up}",'
          f'"location":{{'
            f'"latitude":"{lat}",'
            f'"ns":"{ns}",'
            f'"longitude":"{lon}",'
            f'"ew":"{ew}",'
            f'"altitude_m":"{alt}"'
          f'}},'
          f'"environment":{{'
            f'"temperature_c":"{temp}",'
            f'"humidity_percent":"{hum}",'
            f'"pressure_hpa":"{pres}",'
            f'"gas_ppm":"{gas}"'
          f'}},'
          f'"acceleration":{{'
            f'"x_mps2":"{x}",'
            f'"y_mps2":"{y}",'
            f'"z_mps2":"{z}"'
          f'}}'
        f'}}'
    )

    # 4. turn it into bytes and pad out to 1024 bytes with NULs
    data = snippet.encode('ascii')
    if len(data) + 1 > JSON_BUFFER_SIZE:
        raise ValueError("Formatted JSON too large!")
    # +1 for the C snprintf’s trailing NULL; but since we pad with zeros
    # a simple left-pad works:
    buf = data.ljust(JSON_BUFFER_SIZE, b'\0')

    # 5. compute SHA256
    digest = hashlib.sha256(buf).hexdigest()

    return digest
