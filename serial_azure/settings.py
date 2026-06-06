"""
Load configuration (e.g. server URL) from an INI file.
"""

import os
import configparser

_cfg = configparser.ConfigParser()
_cfg.read(os.path.join(os.path.dirname(__file__), "config.ini"))

SERVER_URL = _cfg["server"]["url"]
