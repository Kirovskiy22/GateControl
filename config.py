import json
from pathlib import Path

CONFIG_FILE = Path(__file__).parent / "config.json"


class Config:

    def __init__(self):
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            self.data = json.load(f)

    @property
    def ip(self):
        return self.data["ip"]

    @property
    def port(self):
        return self.data["port"]

    @property
    def community(self):
        return self.data["community"]

    @property
    def oid(self):
        return self.data["oid_do1"]

    @property
    def do1_mode(self) -> str:
        return self.data.get("do1_mode", "hold")

    @property
    def pulse_cooldown_sec(self) -> float:
        return float(self.data.get("pulse_cooldown_sec", 3.5))

    @property
    def command_cooldown_sec(self) -> float:
        return float(self.data.get("command_cooldown_sec", 0.8))

    @property
    def web_host(self) -> str:
        return self.data.get("web_host", "0.0.0.0")

    @property
    def web_port(self) -> int:
        return int(self.data.get("web_port", 8080))

    @property
    def rtsp_url(self) -> str:
        return str(self.data.get("rtsp_url", "")).strip()

    @property
    def anpr_enabled(self) -> bool:
        return bool(self.data.get("anpr_enabled", False))

    @property
    def anpr_min_confidence(self) -> float:
        return float(self.data.get("anpr_min_confidence", 0.6))

    @property
    def anpr_frame_interval_sec(self) -> float:
        return float(self.data.get("anpr_frame_interval_sec", 0.4))

    @property
    def anpr_open_cooldown_sec(self) -> float:
        return float(self.data.get("anpr_open_cooldown_sec", 20))

    @property
    def anpr_whitelist_only(self) -> bool:
        return bool(self.data.get("anpr_whitelist_only", False))

    @property
    def anpr_require_valid_format(self) -> bool:
        return bool(self.data.get("anpr_require_valid_format", True))

    @property
    def anpr_open_on_detect(self) -> bool:
        return bool(self.data.get("anpr_open_on_detect", True))

    @property
    def anpr_allowed_plates(self) -> list[str]:
        plates = self.data.get("anpr_allowed_plates", [])
        if not isinstance(plates, list):
            return []
        return [str(item) for item in plates]

    @property
    def anpr_rtsp_transport(self) -> str:
        return str(self.data.get("anpr_rtsp_transport", "tcp"))

    @property
    def anpr_resize_width(self) -> int:
        return int(self.data.get("anpr_resize_width", 1280))
