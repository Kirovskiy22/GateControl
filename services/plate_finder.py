from __future__ import annotations

from services.preprocess import gray_plate_masks, prepare_plate_crop, warp_quad_plate

# Приоритетные зоны поиска: верхний центр (камера над воротами), центр, весь кадр.
_SEARCH_REGIONS = (
    (0.10, 0.55, 0.22, 0.78),  # верхний центр — номер на стенде / шлагбауме
    (0.18, 0.82, 0.15, 0.85),  # центральная полоса
    (0.00, 1.00, 0.00, 1.00),  # весь кадр
)
# Узкие окна под типичный размер ГРЗ в кадре (руки / стенд).
_PRESET_PLATE_WINDOWS = (
    (0.20, 0.36, 0.38, 0.68),  # верхний центр — номер на стенде
    (0.45, 0.62, 0.30, 0.66),  # центр — номер в руках
    (0.48, 0.58, 0.34, 0.60),  # ещё уже
)
# Когда кадр уже обрезан по ROI — ищем по всей зоне и её подокнам.
_ROI_SEARCH_REGIONS = ((0.00, 1.00, 0.00, 1.00),)
_ROI_PRESET_PLATE_WINDOWS = (
    (0.00, 0.38, 0.08, 0.92),  # верх ROI — номер на стенде
    (0.05, 0.50, 0.15, 0.85),  # верх ROI — шире
    (0.00, 1.00, 0.00, 1.00),  # вся ROI
    (0.35, 0.95, 0.20, 0.80),  # низ ROI — номер в руках
)


def find_plate_crops(frame, max_crops: int = 4, *, roi_limited: bool = False) -> list:
    """Поиск ГРЗ: маски (Nomeroff) → контуры → перспектива → кроп для OCR."""
    import cv2

    if frame is None or frame.size == 0:
        return []

    height, width = frame.shape[:2]
    candidates: list[tuple[float, tuple[int, int, int, int], object | None]] = []
    search_regions = _ROI_SEARCH_REGIONS if roi_limited else _SEARCH_REGIONS
    preset_windows = _ROI_PRESET_PLATE_WINDOWS if roi_limited else _PRESET_PLATE_WINDOWS

    for y0, y1, x0, x1 in search_regions:
        roi_y1, roi_y2 = int(height * y0), int(height * y1)
        roi_x1, roi_x2 = int(width * x0), int(width * x1)
        roi = frame[roi_y1:roi_y2, roi_x1:roi_x2]
        if roi.size == 0:
            continue
        zone_bias = _zone_bias(y0, y1, x0, x1)
        for score, box, contour in _find_in_roi(roi, zone_bias):
            left, top, right, bottom = box
            full_box = (left + roi_x1, top + roi_y1, right + roi_x1, bottom + roi_y1)
            full_contour = _shift_contour(contour, roi_x1, roi_y1) if contour is not None else None
            candidates.append((score, full_box, full_contour))

    candidates.sort(key=lambda item: item[0], reverse=True)
    crops: list = []
    seen_boxes: set[tuple[int, int, int, int]] = set()

    for y0, y1, x0, x1 in preset_windows:
        left = int(width * x0)
        top = int(height * y0)
        right = int(width * x1)
        bottom = int(height * y1)
        preset = frame[top:bottom, left:right]
        if preset.size == 0:
            continue
        key = (left, top, right, bottom)
        if key in seen_boxes:
            continue
        seen_boxes.add(key)
        crops.append(prepare_plate_crop(preset))
        if len(crops) >= max_crops:
            return crops

    for _, (x1, y1, x2, y2), contour in candidates:
        box_w = max(1, x2 - x1)
        box_h = max(1, y2 - y1)
        pad_left = int(box_w * 0.22)
        pad_right = int(box_w * 0.10)
        pad_y = int(box_h * 0.18)
        left = max(0, x1 - pad_left)
        top = max(0, y1 - pad_y)
        right = min(width, x2 + pad_right)
        bottom = min(height, y2 + pad_y)
        key = (left, top, right, bottom)
        if key in seen_boxes:
            continue
        seen_boxes.add(key)
        crop = frame[top:bottom, left:right]
        if crop.size == 0:
            continue
        ch, cw = crop.shape[:2]
        if cw / max(ch, 1) < 1.8 or ch < 8:
            continue

        warped = None
        if contour is not None:
            warped = warp_quad_plate(frame, contour)
        if warped is not None and warped.size > 0:
            wh, ww = warped.shape[:2]
            if ww / max(wh, 1) >= 2.0:
                crop = warped

        crop = prepare_plate_crop(crop)
        if any(_same_crop(crop, existing) for existing in crops):
            continue
        crops.append(crop)
        if len(crops) >= max_crops:
            break
    return crops


def _zone_bias(y0: float, y1: float, x0: float, x1: float) -> float:
    """Выше и ближе к центру — выше приоритет (типичная камера ворот)."""
    cy = (y0 + y1) / 2
    cx = (x0 + x1) / 2
    vertical = 1.0 - min(1.0, cy / 0.55)
    horizontal = 1.0 - min(1.0, abs(cx - 0.5) / 0.5)
    tightness = 1.0 / max(0.35, (y1 - y0) * (x1 - x0))
    return 0.35 + 0.35 * vertical + 0.2 * horizontal + 0.1 * min(1.5, tightness)


def _find_in_roi(
    roi, zone_bias: float = 1.0, *, allow_upscale: bool = True
) -> list[tuple[float, tuple[int, int, int, int], object | None]]:
    import cv2
    import numpy as np

    height, width = roi.shape[:2]
    if height < 32 or width < 32:
        return []

    min_area = width * height * 0.00035
    max_area = width * height * 0.45
    candidates: list[tuple[float, tuple[int, int, int, int], object | None]] = []

    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)

    masks = _build_masks(roi, gray, blur)
    rect_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (9, 3))
    thin_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 2))

    for mask in masks:
        for kernel, iters in ((rect_kernel, 2), (thin_kernel, 1)):
            closed = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=iters)
            closed = cv2.morphologyEx(closed, cv2.MORPH_OPEN, thin_kernel, iterations=1)
            contours, _ = cv2.findContours(closed, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
            for contour in contours:
                scored = _score_contour(contour, roi, gray, width, height, min_area, max_area)
                if scored is None:
                    continue
                score, box = scored
                score *= zone_bias
                candidates.append((score, box, contour))

    # Дополнительный проход на увеличенном ROI для мелких номеров (Habr: bbox вместо маски).
    if allow_upscale and height < 280 and width < 400:
        scaled = cv2.resize(roi, (width * 2, height * 2), interpolation=cv2.INTER_CUBIC)
        for score, box, contour in _find_in_roi(scaled, zone_bias * 0.92, allow_upscale=False):
            x1, y1, x2, y2 = box
            half_contour = _scale_contour(contour, 0.5) if contour is not None else None
            candidates.append((score * 0.95, (x1 // 2, y1 // 2, x2 // 2, y2 // 2), half_contour))

    candidates.sort(key=lambda item: item[0], reverse=True)
    deduped: list[tuple[float, tuple[int, int, int, int], object | None]] = []
    for score, box, contour in candidates:
        if any(_iou(box, existing) > 0.55 for _, existing, _ in deduped):
            continue
        deduped.append((score, box, contour))
    return deduped


def _build_masks(roi, gray, blur):
    import cv2

    hsv = cv2.cvtColor(roi, cv2.COLOR_BGR2HSV)
    masks = list(gray_plate_masks(gray, blur))
    masks.extend([
        cv2.inRange(hsv, (0, 0, 160), (180, 70, 255)),
        cv2.inRange(hsv, (0, 0, 140), (180, 50, 255)),
    ])

    # Black-hat: светлая пластина на тёмном фоне (Nomeroff: бинарная маска контура).
    blackhat = cv2.morphologyEx(
        gray, cv2.MORPH_BLACKHAT, cv2.getStructuringElement(cv2.MORPH_RECT, (21, 7))
    )
    _, bh_mask = cv2.threshold(blackhat, 12, 255, cv2.THRESH_BINARY)
    masks.append(bh_mask)

    # Горизонтальные границы символов (smeyanoff/YOLO: рамка как прямоугольник).
    grad_x = cv2.Sobel(blur, cv2.CV_16S, 1, 0, ksize=3)
    grad_x = cv2.convertScaleAbs(grad_x)
    _, edge_mask = cv2.threshold(grad_x, 40, 255, cv2.THRESH_BINARY)
    masks.append(edge_mask)

    canny = cv2.Canny(blur, 50, 150)
    masks.append(canny)

    return masks


def _score_contour(contour, roi, gray, width, height, min_area, max_area):
    import cv2

    area = cv2.contourArea(contour)
    if area < min_area or area > max_area:
        return None

    x, y, bw, bh = cv2.boundingRect(contour)
    if bh < 8 or bw < 20:
        return None

    ratio = bw / max(1, bh)
    if ratio < 2.0 or ratio > 8.5:
        return None

    hull = cv2.convexHull(contour)
    hull_area = max(1.0, cv2.contourArea(hull))
    solidity = area / hull_area
    if solidity < 0.45:
        return None

    rect_area = bw * bh
    extent = area / max(1.0, rect_area)
    if extent < 0.35:
        return None

    peri = cv2.arcLength(contour, True)
    approx = cv2.approxPolyDP(contour, 0.04 * peri, True)
    quad_bonus = 1.15 if len(approx) == 4 else 1.0

    patch = gray[y : y + bh, x : x + bw]
    if patch.size == 0:
        return None
    brightness = float(patch.mean()) / 255.0
    contrast = float(patch.std()) / 64.0
    if brightness < 0.38:
        return None

    cx = x + bw / 2
    cy = y + bh / 2
    center_bias = 1.0 - (
        abs(cx - width / 2) / (width / 2) + abs(cy - height / 2) / (height / 2)
    ) / 2
    wide_bonus = min(1.35, ratio / 3.2)

    score = (
        area
        * (0.45 + 0.55 * max(0.0, center_bias))
        * wide_bonus
        * quad_bonus
        * (0.6 + 0.4 * min(1.0, brightness))
        * (0.7 + 0.3 * min(1.0, contrast))
        * (0.5 + 0.5 * solidity)
    )
    return score, (x, y, x + bw, y + bh)


def _shift_contour(contour, dx: int, dy: int):
    shifted = contour.copy()
    shifted[:, 0, 0] += dx
    shifted[:, 0, 1] += dy
    return shifted


def _scale_contour(contour, factor: float):
    scaled = contour.astype("float32").copy()
    scaled[:, 0, :] *= factor
    return scaled.astype(contour.dtype)


def _iou(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> float:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    inter = max(0, ix2 - ix1) * max(0, iy2 - iy1)
    if inter <= 0:
        return 0.0
    union = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - inter
    return inter / max(1, union)


def _same_crop(a, b, tol: float = 0.2) -> bool:
    ha, wa = a.shape[:2]
    hb, wb = b.shape[:2]
    if abs(wa - wb) / max(wa, 1) > tol or abs(ha - hb) / max(ha, 1) > tol:
        return False
    return abs(wa * ha - wb * hb) / max(wa * ha, 1) < tol
