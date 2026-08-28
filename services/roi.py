from __future__ import annotations

from typing import TypedDict


class RoiDict(TypedDict):
    x0: float
    y0: float
    x1: float
    y1: float


# Зона по умолчанию (старый стенд с текстом). При anpr_roi_enabled: false не используется.
DEFAULT_ANPR_ROI: RoiDict = {
    "x0": 0.40,
    "y0": 0.08,
    "x1": 1.0,
    "y1": 1.0,
}


def parse_roi(data: dict | None) -> RoiDict:
    """Нормализованная ROI (0..1) с подстановкой значений по умолчанию."""
    merged: dict[str, float] = dict(DEFAULT_ANPR_ROI)
    if isinstance(data, dict):
        for key in ("x0", "y0", "x1", "y1"):
            if key in data:
                merged[key] = float(data[key])
    return clamp_roi(merged)


def clamp_roi(roi: dict) -> RoiDict:
    """Ограничить координаты [0, 1] и гарантировать минимальный размер."""
    x0 = max(0.0, min(1.0, float(roi.get("x0", DEFAULT_ANPR_ROI["x0"]))))
    y0 = max(0.0, min(1.0, float(roi.get("y0", DEFAULT_ANPR_ROI["y0"]))))
    x1 = max(0.0, min(1.0, float(roi.get("x1", DEFAULT_ANPR_ROI["x1"]))))
    y1 = max(0.0, min(1.0, float(roi.get("y1", DEFAULT_ANPR_ROI["y1"]))))

    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0

    min_span = 0.05
    if x1 - x0 < min_span:
        cx = (x0 + x1) / 2
        x0 = max(0.0, cx - min_span / 2)
        x1 = min(1.0, cx + min_span / 2)
    if y1 - y0 < min_span:
        cy = (y0 + y1) / 2
        y0 = max(0.0, cy - min_span / 2)
        y1 = min(1.0, cy + min_span / 2)

    return RoiDict(x0=x0, y0=y0, x1=x1, y1=y1)


def roi_pixel_box(shape, roi: dict) -> tuple[int, int, int, int]:
    """ROI в пикселях: left, top, right, bottom (right/bottom — exclusive)."""
    height, width = shape[:2]
    box = clamp_roi(roi)
    left = int(width * box["x0"])
    top = int(height * box["y0"])
    right = int(width * box["x1"])
    bottom = int(height * box["y1"])

    left = max(0, min(width - 1, left))
    top = max(0, min(height - 1, top))
    right = max(left + 1, min(width, right))
    bottom = max(top + 1, min(height, bottom))
    return left, top, right, bottom


def crop_roi(frame, roi: dict):
    """Вырезать ROI из кадра BGR."""
    if frame is None or frame.size == 0:
        return frame
    left, top, right, bottom = roi_pixel_box(frame.shape, roi)
    return frame[top:bottom, left:right].copy()


def draw_roi(frame, roi: dict, *, color=(0, 220, 80), thickness: int = 2):
    """Нарисовать рамку ROI на кадре (in-place)."""
    import cv2

    left, top, right, bottom = roi_pixel_box(frame.shape, roi)
    cv2.rectangle(frame, (left, top), (right - 1, bottom - 1), color, thickness)
    return frame
