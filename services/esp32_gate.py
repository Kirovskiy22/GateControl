"""HTTP client for the single START relay input on ESP32."""
from __future__ import annotations

import json
import os
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from config import Config


class ESP32GateController:
    """Sends short START/STOP requests; it cannot infer the gate position."""

    def __init__(self, cfg: Config | None = None):
        self.cfg = cfg or Config()
        self._lock = threading.Lock()
        self._last_command_at = 0.0
        self._last_command: str | None = None
        self._last_command_at_text: str | None = None
        self._device: dict = {}

    def _request(self, endpoint: str) -> dict:
        base = self.cfg.esp32_base_url.rstrip("/")
        if not base.startswith(("http://", "https://")):
            raise ValueError("esp32_base_url должен начинаться с http:// или https://")
        headers = {"Accept": "application/json", "Connection": "close"}
        token = self.cfg.esp32_token
        if token:
            headers["X-Token"] = token
        request = Request(f"{base}/{endpoint.lstrip('/')}", headers=headers, method="GET")
        try:
            with urlopen(request, timeout=self.cfg.esp32_timeout_sec) as response:
                raw = response.read(8192).decode("utf-8", errors="replace")
                if response.status < 200 or response.status >= 300:
                    raise RuntimeError(f"ESP32 HTTP {response.status}")
        except HTTPError as exc:
            if exc.code == 401:
                raise RuntimeError("ESP32 отклонил токен (HTTP 401)") from exc
            raise RuntimeError(f"ESP32 HTTP {exc.code}") from exc
        except (URLError, TimeoutError, OSError) as exc:
            raise RuntimeError(f"Нет связи с ESP32: {exc}") from exc
        try:
            data = json.loads(raw) if raw else {}
        except json.JSONDecodeError as exc:
            raise RuntimeError("ESP32 вернул некорректный JSON") from exc
        if not isinstance(data, dict) or data.get("ok") is False:
            raise RuntimeError(str(data.get("error", "ESP32 отклонил команду")))
        return data

    def _command(self, endpoint: str, label: str) -> tuple[bool, str | None]:
        with self._lock:
            now = time.monotonic()
            wait = self.cfg.esp32_command_cooldown_sec - (now - self._last_command_at)
            if wait > 0:
                time.sleep(wait)
            try:
                data = self._request(endpoint)
            except Exception as exc:
                return False, str(exc)
            self._last_command_at = time.monotonic()
            self._last_command = label
            self._last_command_at_text = time.strftime("%Y-%m-%d %H:%M:%S")
            self._device = data
            return True, f"ESP32 принял команду {label}; положение ворот не подтверждено"

    def open_gate(self) -> tuple[bool, str | None]:
        # The physical IO4 is wired to START. Repeated pulses toggle the operator.
        return self._command("open", "START / импульс")

    def stop_gate(self) -> tuple[bool, str | None]:
        # /stop only releases ESP32 outputs; it stops the motor only if STOP is wired.
        return self._command("stop", "STOP / снять выходы")

    def close_gate(self) -> tuple[bool, str | None]:
        # Compatibility alias: the installed hardware has no dedicated CLOSE wire.
        return False, "Отдельный CLOSE не подключён. Используйте импульс START вручную."

    def refresh(self) -> tuple[bool, str | None]:
        try:
            self._device = self._request("status")
            return True, "Связь с ESP32 проверена; положение ворот неизвестно"
        except Exception as exc:
            return False, str(exc)

    def get_status(self) -> dict:
        return {
            "state": None,
            "label": "положение ворот неизвестно",
            "controller": "esp32",
            "esp32": {
                "url": self.cfg.esp32_base_url,
                "reachable": bool(self._device),
                "fw": self._device.get("fw"),
                "ip": self._device.get("ip"),
                "eth": self._device.get("eth"),
                "hold_mode": self._device.get("hold_mode"),
                "pins": self._device.get("pins"),
                "last_command": self._last_command,
                "last_command_at": self._last_command_at_text,
            },
        }
