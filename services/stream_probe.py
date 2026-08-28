from __future__ import annotations

import time
from dataclasses import dataclass


@dataclass(frozen=True)
class StreamProfile:
    width: int
    height: int
    reported_fps: float
    measured_fps: float
    megapixels: float
    tier: str
    process_interval_sec: float
    preview_interval_sec: float
    resize_width: int
    buffer_drain_grabs: int

    def summary(self) -> str:
        fps = self.measured_fps or self.reported_fps
        return (
            f"{self.width}×{self.height}, ~{fps:.0f} fps, "
            f"анализ каждые {self.process_interval_sec:.1f} с"
        )

    def as_dict(self) -> dict:
        fps = self.measured_fps or self.reported_fps
        return {
            "width": self.width,
            "height": self.height,
            "fps": round(fps, 1),
            "megapixels": round(self.megapixels, 2),
            "tier": self.tier,
            "process_interval_sec": self.process_interval_sec,
            "preview_interval_sec": self.preview_interval_sec,
            "resize_width": self.resize_width,
        }


def tune_stream(
    width: int,
    height: int,
    fps: float,
    *,
    min_process_interval: float = 0.8,
    max_resize_width: int = 1280,
) -> StreamProfile:
    """Подбор интервалов по разрешению и частоте кадров потока."""
    width = max(1, width)
    height = max(1, height)
    fps = fps if fps > 1.0 else 15.0
    megapixels = (width * height) / 1_000_000

    if megapixels <= 0.9:
        tier = "light"
        resize = min(width, 960)
        process = 0.8
        preview = 2.5
        drain = 2
    elif megapixels <= 2.5:
        tier = "medium"
        resize = min(width, max_resize_width)
        process = 1.2
        preview = 3.0
        drain = 3
    else:
        tier = "heavy"
        resize = min(width, max_resize_width)
        process = 2.2
        preview = 3.5
        drain = 4

    if fps >= 20:
        process = max(process, fps / 12.0)
    if fps >= 30:
        drain = min(8, drain + 2)

    process = max(min_process_interval, process)
    resize = max(640, min(resize, max_resize_width))

    return StreamProfile(
        width=width,
        height=height,
        reported_fps=fps,
        measured_fps=fps,
        megapixels=megapixels,
        tier=tier,
        process_interval_sec=round(process, 2),
        preview_interval_sec=round(preview, 2),
        resize_width=resize,
        buffer_drain_grabs=drain,
    )


def probe_capture(cap, sample_sec: float = 1.5) -> StreamProfile:
    """Снять метрики с открытого VideoCapture, не декодируя каждый кадр."""
    import cv2

    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 0)
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 0)
    reported = float(cap.get(cv2.CAP_PROP_FPS) or 0)

    start = time.monotonic()
    grabs = 0
    while time.monotonic() - start < sample_sec:
        if not cap.grab():
            break
        grabs += 1

    elapsed = max(0.1, time.monotonic() - start)
    measured = grabs / elapsed if grabs else 0.0
    if measured < 1.0 and reported > 1.0:
        measured = reported
    elif measured < 1.0:
        measured = 15.0

    if width <= 0 or height <= 0:
        ok, frame = cap.retrieve()
        if ok and frame is not None:
            height, width = frame.shape[:2]
        else:
            width, height = 1280, 720

    profile = tune_stream(width, height, measured)
    return StreamProfile(
        width=width,
        height=height,
        reported_fps=reported,
        measured_fps=round(measured, 1),
        megapixels=profile.megapixels,
        tier=profile.tier,
        process_interval_sec=profile.process_interval_sec,
        preview_interval_sec=profile.preview_interval_sec,
        resize_width=profile.resize_width,
        buffer_drain_grabs=profile.buffer_drain_grabs,
    )
