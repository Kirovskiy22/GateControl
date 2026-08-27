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