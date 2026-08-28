from __future__ import annotations

import logging
import os
import threading
import time
from collections import deque
from datetime import datetime
from typing import Callable

from config import Config
from services.motion import downscale_gray, frame_motion_score
from services.plate import (
    AutoCloseTracker,
    OpenCooldown,
    PlateAccessPolicy,
    format_plate,
    normalize_plate,
)
from services.plate_reader import PlateReader
from services.roi import crop_roi, draw_roi
from services.stream_probe import StreamProfile, probe_capture, tune_stream

logger = logging.getLogger("gatecontrol.anpr")

OpenGateFn = Callable[[], tuple[bool, str | None]]
CloseGateFn = Callable[[], tuple[bool, str | None]]


class AnprWorker:
    """Фоновый разбор RTSP: номер → решение → открытие шлагбаума."""

    def __init__(
        self,
        open_gate: OpenGateFn,
        close_gate: CloseGateFn | None = None,
        cfg: Config | None = None,
    ):
        self.cfg = cfg or Config()
        self._open_gate = open_gate
        self._close_gate = close_gate
        self._policy = PlateAccessPolicy(
            self.cfg.anpr_allowed_plates,
            whitelist_only=self.cfg.anpr_whitelist_only,
            require_valid_format=self.cfg.anpr_require_valid_format,
            min_confidence=self.cfg.anpr_min_confidence,
            open_min_confidence=self.cfg.anpr_open_min_confidence,
            require_region=self.cfg.anpr_require_region,
            open_on_detect=self.cfg.anpr_open_on_detect,
        )
        self._cooldown = OpenCooldown(self.cfg.anpr_open_cooldown_sec)
        self._auto_close = AutoCloseTracker(
            self.cfg.anpr_close_after_sec,
            enabled=self.cfg.anpr_auto_close,
        )
        self._reader = PlateReader(
            easyocr_enabled=self.cfg.anpr_easyocr_enabled,
            cv_fallback=self.cfg.anpr_cv_fallback,
        )

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
        self._reconnect = threading.Event()
        self._url_lock = threading.Lock()
        self._stream_profile: StreamProfile | None = None
        self._last_plate_monotonic: float | None = None

    def set_stream(self, url: str, flip: bool | None = None) -> tuple[bool, str]:
        url = url.strip()
        lowered = url.lower()
        if not lowered.startswith(("rtsp://", "http://", "https://")):
            return False, "Нужен URL вида rtsp://... или http://..."

        with self._url_lock:
            self.cfg.set_rtsp_url(url)
            if flip is not None:
                self.cfg.set_flip_horizontal(flip)
        try:
            self.cfg.save()
        except OSError as exc:
            logger.warning("Не удалось сохранить config.json: %s", exc)

        self._set_state(camera="connecting", error=None)
        with self._lock:
            self._jpeg = None
            self._stream_profile = None
        if self._thread and self._thread.is_alive():
            self._reconnect.set()
            self._push_event("Переключение RTSP-потока", "info")
        else:
            self.start()
        return True, "Подключаюсь к камере"

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

        self._thread = None
        self._stop.clear()

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
                "rtsp_url": self.cfg.rtsp_url,
                "flip_horizontal": self.cfg.anpr_flip_horizontal,
                "has_frame": self._jpeg is not None,
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
                "stream": self._stream_profile.as_dict() if self._stream_profile else None,
                "roi": {
                    "enabled": self.cfg.anpr_roi_enabled,
                    **self.cfg.anpr_roi,
                },
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
                if self._wait_backoff(backoff):
                    break
                if self._reconnect.is_set():
                    self._reconnect.clear()
                    backoff = 1.0
                    continue
                backoff = min(backoff * 2.0, 15.0)
                continue

            backoff = 1.0
            self._set_state(camera="connected", error=None)
            self._push_event("RTSP-поток подключен", "success")
            switched = self._process_stream(cap)
            cap.release()
            if self._stop.is_set():
                break
            if switched or self._reconnect.is_set():
                self._reconnect.clear()
                self._set_state(camera="connecting", error=None)
                continue
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
            error="Не удалось открыть RTSP-поток (проверьте ссылку — у Goodline она живёт ~60 мин)",
        )
        self._push_event(
            "Не удалось открыть RTSP — получите новую ссылку в приложении камеры",
            "error",
        )
        return None

    def _wait_backoff(self, timeout: float) -> bool:
        """Ждать timeout секунд. True — запрошена остановка воркера."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._stop.is_set() or self._reconnect.is_set():
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            if self._stop.wait(min(0.25, remaining)):
                break
        return self._stop.is_set()

    def _process_stream(self, cap) -> bool:
        """True — нужно сразу открыть новый URL, кадры не оборвались."""
        profile = self._build_stream_profile(cap)
        with self._lock:
            self._stream_profile = profile
        self._push_event(f"Поток: {profile.summary()} ({profile.tier})", "info")

        process_interval = profile.process_interval_sec
        preview_interval = profile.preview_interval_sec
        resize_width = profile.resize_width
        drain_grabs = profile.buffer_drain_grabs
        last_process = 0.0
        last_preview = 0.0
        last_analyze_gray = None
        stream_started = time.monotonic()
        self._last_plate_monotonic = None

        while not self._stop.is_set():
            if self._reconnect.is_set():
                return True

            for _ in range(drain_grabs):
                if not cap.grab():
                    return False

            now = time.monotonic()
            need_preview = now - last_preview >= preview_interval
            need_process = now - last_process >= process_interval
            if not need_preview and not need_process:
                time.sleep(0.03)
                continue

            ok, frame = cap.retrieve()
            if not ok or frame is None:
                return False

            frame = self._maybe_flip(frame)
            frame = self._resize_frame(frame, resize_width)

            if need_preview:
                last_preview = now
                self._store_preview(frame)

            if need_process:
                last_process = now
                gray = downscale_gray(frame) if self.cfg.anpr_motion_detect else None
                if self._should_run_alpr(now, stream_started, last_analyze_gray, gray):
                    if gray is not None:
                        last_analyze_gray = gray
                    try:
                        self._handle_frame(frame, now)
                    except Exception as exc:
                        logger.exception("Ошибка разбора кадра")
                        self._push_event(f"Ошибка разбора кадра: {exc}", "error")

        return False

    def _should_run_alpr(
        self,
        now: float,
        stream_started: float,
        last_analyze_gray,
        gray,
    ) -> bool:
        if not self.cfg.anpr_motion_detect:
            return True

        if last_analyze_gray is None or gray is None:
            return True

        score = frame_motion_score(last_analyze_gray, gray)
        if score >= self.cfg.anpr_motion_threshold:
            return True

        retry_sec = self.cfg.anpr_motion_retry_sec
        if retry_sec <= 0:
            return False

        last_plate = self._last_plate_monotonic
        if last_plate is None:
            return (now - stream_started) >= retry_sec
        return (now - last_plate) >= retry_sec

    def _build_stream_profile(self, cap) -> StreamProfile:
        if self.cfg.anpr_auto_tune:
            profile = probe_capture(cap)
            tuned = tune_stream(
                profile.width,
                profile.height,
                profile.measured_fps or profile.reported_fps,
                min_process_interval=max(0.5, self.cfg.anpr_frame_interval_sec),
                max_resize_width=max(640, self.cfg.anpr_resize_width),
            )
            return StreamProfile(
                width=profile.width,
                height=profile.height,
                reported_fps=profile.reported_fps,
                measured_fps=profile.measured_fps,
                megapixels=tuned.megapixels,
                tier=tuned.tier,
                process_interval_sec=tuned.process_interval_sec,
                preview_interval_sec=tuned.preview_interval_sec,
                resize_width=tuned.resize_width,
                buffer_drain_grabs=tuned.buffer_drain_grabs,
            )

        try:
            import cv2

            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 1280)
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 720)
        except Exception:
            width, height = 1280, 720
        return tune_stream(
            width,
            height,
            15.0,
            min_process_interval=max(0.5, self.cfg.anpr_frame_interval_sec),
            max_resize_width=max(640, self.cfg.anpr_resize_width),
        )

    def _is_meaningful_read(self, read) -> bool:
        text = normalize_plate(read.text)
        if len(text) < 4:
            return False
        return read.confidence >= self.cfg.anpr_min_confidence

    def _should_log_decision(self, decision) -> bool:
        if not decision.plate or len(decision.plate) < 4:
            return False
        if decision.confidence < self.cfg.anpr_min_confidence:
            return False
        return True

    def _handle_frame(self, frame, now: float) -> None:
        alpr_frame = self._frame_for_alpr(frame)
        reads = self._reader.read(alpr_frame, roi_limited=self.cfg.anpr_roi_enabled)
        meaningful = [item for item in reads if self._is_meaningful_read(item)]
        if not meaningful:
            self._maybe_auto_close(now)
            return

        self._auto_close.observe_plate()
        self._last_plate_monotonic = now
        best = meaningful[0]
        decision = self._policy.evaluate(best.text, best.confidence)
        self._remember_plate(decision.plate, decision.confidence, decision.reason)

        shown = format_plate(decision.plate) if decision.plate else ""
        if not decision.open_gate:
            if not self._should_log_decision(decision):
                return
            key = (decision.plate, decision.reason)
            if key != self._last_logged:
                self._last_logged = key
                self._maybe_save_debug_crop(alpr_frame, decision.plate, decision.reason)
                self._push_event(
                    f"{shown or decision.plate}: {decision.reason} ({decision.confidence:.0%})",
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
            self._auto_close.observe_plate()
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

    def _maybe_auto_close(self, now: float) -> None:
        if self._close_gate is None:
            return
        if not self._auto_close.should_close(now, self._cooldown.remaining(now)):
            return

        ok, message = self._close_gate()
        self._auto_close.mark_closed()
        if ok:
            detail = message or "номер не в кадре"
            self._remember_closed(detail)
            self._push_event(f"Шлагбаум опущен ({detail})", "info")
            return

        self._remember_closed(message or "не удалось опустить")
        self._push_event(f"Не удалось опустить шлагбаум — {message}", "error")

    def _maybe_flip(self, frame):
        import cv2

        if not self.cfg.anpr_flip_horizontal:
            return frame
        return cv2.flip(frame, 1)

    def _resize_frame(self, frame, width: int):
        import cv2

        if width <= 0:
            return frame
        current_w = frame.shape[1]
        if current_w <= width:
            return frame
        scale = width / current_w
        size = (width, int(frame.shape[0] * scale))
        return cv2.resize(frame, size, interpolation=cv2.INTER_AREA)

    def _maybe_resize(self, frame):
        width = self.cfg.anpr_resize_width
        if self._stream_profile is not None:
            width = self._stream_profile.resize_width
        return self._resize_frame(frame, width)

    def _frame_for_alpr(self, frame):
        if not self.cfg.anpr_roi_enabled:
            return frame
        cropped = crop_roi(frame, self.cfg.anpr_roi)
        if cropped is None or cropped.size == 0:
            return frame
        return self._upscale_small_alpr_frame(cropped)

    def _upscale_small_alpr_frame(self, frame, min_width: int = 960):
        """ROI после resize потока часто мелкая — увеличить перед ALPR."""
        import cv2

        width = frame.shape[1]
        if width >= min_width:
            return frame
        scale = min_width / max(1, width)
        size = (min_width, max(1, int(frame.shape[0] * scale)))
        return cv2.resize(frame, size, interpolation=cv2.INTER_CUBIC)

    def _maybe_save_debug_crop(self, frame, plate: str, reason: str) -> None:
        """Сохранить кадр ALPR при отклонении распознанного текста (диагностика)."""
        if not plate or "российский" not in reason:
            return
        try:
            import cv2

            cv2.imwrite("debug_last_alpr_crop.jpg", frame)
        except OSError:
            logger.debug("Не удалось сохранить debug_last_alpr_crop.jpg")

    def _store_preview(self, frame) -> None:
        import cv2

        preview = frame.copy()
        if self.cfg.anpr_roi_enabled:
            draw_roi(preview, self.cfg.anpr_roi)
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
            if reason.startswith("открыт"):
                self._last_decision = "opened"
            elif reason.startswith("опущен"):
                self._last_decision = "closed"
            else:
                self._last_decision = "ignored"
            self._last_reason = reason
            self._last_at = datetime.now().strftime("%H:%M:%S")

    def _remember_closed(self, reason: str) -> None:
        with self._lock:
            self._last_decision = "closed"
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
