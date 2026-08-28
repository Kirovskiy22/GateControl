from __future__ import annotations

import logging
import os
import threading
import time
from collections import deque
from datetime import datetime
from typing import Callable

from config import Config
from services.plate import OpenCooldown, PlateAccessPolicy, format_plate
from services.plate_reader import PlateReader

logger = logging.getLogger("gatecontrol.anpr")

OpenGateFn = Callable[[], tuple[bool, str | None]]


class AnprWorker:
    """Фоновый разбор RTSP: номер → решение → открытие шлагбаума."""

    def __init__(self, open_gate: OpenGateFn, cfg: Config | None = None):
        self.cfg = cfg or Config()
        self._open_gate = open_gate
        self._policy = PlateAccessPolicy(
            self.cfg.anpr_allowed_plates,
            whitelist_only=self.cfg.anpr_whitelist_only,
            require_valid_format=self.cfg.anpr_require_valid_format,
            min_confidence=self.cfg.anpr_min_confidence,
            open_on_detect=self.cfg.anpr_open_on_detect,
        )
        self._cooldown = OpenCooldown(self.cfg.anpr_open_cooldown_sec)
        self._reader = PlateReader()

        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self._event_id = 0
        self._events: deque[dict] = deque(maxlen=50)
        self._jpeg: bytes | None = None

        self._camera = "disabled"
        self._reader_state = "disabled"
        self._error: str | None = None
        self._last_plate: str | None = None
        self._last_confidence: float | None = None
        self._last_decision: str | None = None
        self._last_reason: str | None = None
        self._last_at: str | None = None
        self._last_logged: tuple[str, str] | None = None

    def start(self) -> None:
        if not self.cfg.anpr_enabled:
            self._set_state(camera="disabled", reader="disabled", error=None)
            self._push_event("Распознавание номеров выключено", "info")
            return

        if not self.cfg.rtsp_url:
            self._set_state(
                camera="disconnected",
                reader="error",
                error="Не задан rtsp_url в config.json",
            )
            self._push_event("Не задан rtsp_url — камера не запущена", "error")
            return

        if self._thread and self._thread.is_alive():
            return

        try:
            import cv2  # noqa: F401
        except ImportError:
            self._set_state(
                camera="disconnected",
                reader="error",
                error="Не установлен opencv-python-headless",
            )
            self._push_event(
                "Установите зависимости: pip install -r requirements.txt",
                "error",
            )
            return

        transport = self.cfg.anpr_rtsp_transport.lower()
        if transport == "tcp":
            os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"

        self._stop.clear()
        self._set_state(camera="connecting", reader="loading", error=None)
        self._thread = threading.Thread(
            target=self._run,
            name="AnprWorker",
            daemon=True,
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread and thread.is_alive():
            thread.join(timeout=5)
        self._thread = None

    def last_jpeg(self) -> bytes | None:
        with self._lock:
            return self._jpeg

    def get_status(self) -> dict:
        with self._lock:
            return {
                "enabled": self.cfg.anpr_enabled,
                "camera": self._camera,
                "reader": self._reader_state,
                "error": self._error,
                "last_plate": self._last_plate,
                "last_plate_display": format_plate(self._last_plate or "")
                if self._last_plate
                else None,
                "last_confidence": self._last_confidence,
                "last_decision": self._last_decision,
                "last_reason": self._last_reason,
                "last_at": self._last_at,
                "events": list(self._events),
            }

    def _run(self) -> None:
        try:
            self._push_event("Загрузка модели распознавания номеров...", "info")
            self._reader.load()
            self._set_state(reader="ready", error=None)
            self._push_event("Модель распознавания готова", "success")
        except Exception as exc:
            logger.exception("Не удалось загрузить ALPR")
            self._set_state(camera="disconnected", reader="error", error=str(exc))
            self._push_event(f"Ошибка загрузки модели: {exc}", "error")
            return

        backoff = 1.0
        while not self._stop.is_set():
            cap = self._open_capture()
            if cap is None:
                self._set_state(camera="disconnected")
                if self._stop.wait(backoff):
                    break
                backoff = min(backoff * 2.0, 15.0)
                continue

            backoff = 1.0
            self._set_state(camera="connected", error=None)
            self._push_event("RTSP-поток подключен", "success")
            self._process_stream(cap)
            cap.release()
            if not self._stop.is_set():
                self._set_state(camera="disconnected", error="Поток камеры оборвался")
                self._push_event("RTSP-поток оборвался, переподключение...", "warn")

    def _open_capture(self):
        import cv2

        cap = cv2.VideoCapture(self.cfg.rtsp_url, cv2.CAP_FFMPEG)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        if cap.isOpened():
            return cap
        cap.release()
        self._set_state(
            camera="disconnected",
            error="Не удалось открыть RTSP-поток",
        )
        logger.warning("Не удалось открыть RTSP: %s", self.cfg.rtsp_url)
        return None

    def _process_stream(self, cap) -> None:
        interval = max(0.15, self.cfg.anpr_frame_interval_sec)
        last_process = 0.0

        while not self._stop.is_set():
            grabbed = cap.grab()
            if not grabbed:
                return

            now = time.monotonic()
            if now - last_process < interval:
                continue
            last_process = now

            ok, frame = cap.retrieve()
            if not ok or frame is None:
                return

            frame = self._maybe_flip(frame)
            frame = self._maybe_resize(frame)
            self._store_preview(frame)

            try:
                self._handle_frame(frame, now)
            except Exception as exc:
                logger.exception("Ошибка разбора кадра")
                self._push_event(f"Ошибка разбора кадра: {exc}", "error")

    def _handle_frame(self, frame, now: float) -> None:
        reads = self._reader.read(frame)
        if not reads:
            return

        best = reads[0]
        decision = self._policy.evaluate(best.text, best.confidence)
        self._remember_plate(decision.plate, decision.confidence, decision.reason)

        shown = format_plate(decision.plate)
        if not decision.open_gate:
            key = (decision.plate, decision.reason)
            if key != self._last_logged:
                self._last_logged = key
                self._push_event(
                    f"{shown}: {decision.reason} ({decision.confidence:.0%})",
                    decision.kind,
                )
            return

        blocked, why = self._cooldown.blocked(now)
        if blocked:
            self._remember_plate(decision.plate, decision.confidence, why)
            return

        ok, message = self._open_gate()
        if ok:
            self._cooldown.mark(decision.plate, now)
            detail = message or decision.reason
            self._remember_plate(decision.plate, decision.confidence, f"открыт: {detail}")
            self._push_event(
                f"{shown}: шлагбаум открыт ({decision.confidence:.0%})",
                "success",
            )
            return

        self._remember_plate(decision.plate, decision.confidence, message or "не удалось открыть")
        self._push_event(
            f"{shown}: не удалось открыть — {message}",
            "error",
        )

    def _maybe_flip(self, frame):
        import cv2

        if not self.cfg.anpr_flip_horizontal:
            return frame
        return cv2.flip(frame, 1)

    def _maybe_resize(self, frame):
        import cv2

        width = self.cfg.anpr_resize_width
        if width <= 0:
            return frame
        current_w = frame.shape[1]
        if current_w <= width:
            return frame
        scale = width / current_w
        size = (width, int(frame.shape[0] * scale))
        return cv2.resize(frame, size, interpolation=cv2.INTER_AREA)

    def _store_preview(self, frame) -> None:
        import cv2

        preview = frame
        max_w = 640
        if preview.shape[1] > max_w:
            scale = max_w / preview.shape[1]
            preview = cv2.resize(
                preview,
                (max_w, int(preview.shape[0] * scale)),
                interpolation=cv2.INTER_AREA,
            )
        ok, buf = cv2.imencode(".jpg", preview, [int(cv2.IMWRITE_JPEG_QUALITY), 70])
        if not ok:
            return
        with self._lock:
            self._jpeg = buf.tobytes()

    def _remember_plate(self, plate: str, confidence: float, reason: str) -> None:
        with self._lock:
            self._last_plate = plate
            self._last_confidence = round(confidence, 3)
            self._last_decision = "opened" if reason.startswith("открыт") else "ignored"
            self._last_reason = reason
            self._last_at = datetime.now().strftime("%H:%M:%S")

    def _set_state(
        self,
        *,
        camera: str | None = None,
        reader: str | None = None,
        error: str | None | object = ...,
    ) -> None:
        with self._lock:
            if camera is not None:
                self._camera = camera
            if reader is not None:
                self._reader_state = reader
            if error is not ...:
                self._error = error

    def _push_event(self, text: str, kind: str) -> None:
        with self._lock:
            self._event_id += 1
            event = {
                "id": self._event_id,
                "time": datetime.now().strftime("%H:%M:%S"),
                "text": text,
                "kind": kind,
            }
            self._events.appendleft(event)
        logger.info("%s", text)
