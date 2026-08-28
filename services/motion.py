from __future__ import annotations

import numpy as np


def downscale_gray(frame, width: int = 160):
    """BGR frame → small grayscale array for cheap diff."""
    import cv2

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape[:2]
    if w > width:
        scale = width / w
        gray = cv2.resize(
            gray,
            (width, max(1, int(h * scale))),
            interpolation=cv2.INTER_AREA,
        )
    return gray


def frame_motion_score(prev_gray, curr_gray) -> float:
    """Mean absolute pixel difference, normalized 0..1."""
    if prev_gray.shape != curr_gray.shape:
        return 1.0
    diff = np.abs(prev_gray.astype(np.float32) - curr_gray.astype(np.float32))
    return float(diff.mean() / 255.0)


def has_motion(prev_gray, curr_gray, threshold: float) -> bool:
    return frame_motion_score(prev_gray, curr_gray) >= threshold
