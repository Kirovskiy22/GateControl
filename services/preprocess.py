from __future__ import annotations


def pad_box(
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    width: int,
    height: int,
    pad: float = 0.18,
    *,
    pad_left: float | None = None,
    pad_right: float | None = None,
    pad_top: float | None = None,
    pad_bottom: float | None = None,
) -> tuple[int, int, int, int]:
    box_w = max(1.0, x2 - x1)
    box_h = max(1.0, y2 - y1)
    px_left = box_w * (pad_left if pad_left is not None else pad)
    px_right = box_w * (pad_right if pad_right is not None else pad)
    py_top = box_h * (pad_top if pad_top is not None else pad)
    py_bottom = box_h * (pad_bottom if pad_bottom is not None else pad)
    left = max(0, int(x1 - px_left))
    top = max(0, int(y1 - py_top))
    right = min(width, int(x2 + px_right))
    bottom = min(height, int(y2 + py_bottom))
    if right <= left or bottom <= top:
        return 0, 0, width, height
    return left, top, right, bottom


def clahe_bgr(frame):
    import cv2

    lab = cv2.cvtColor(frame, cv2.COLOR_BGR2LAB)
    light, a_ch, b_ch = cv2.split(lab)
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    light = clahe.apply(light)
    return cv2.cvtColor(cv2.merge((light, a_ch, b_ch)), cv2.COLOR_LAB2BGR)


def clahe_gray(gray):
    """CLAHE на сером канале — только для поиска рамки, не для OCR."""
    import cv2

    clahe = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(8, 8))
    return clahe.apply(gray)


def gray_plate_masks(gray, blur):
    """Ч/б-маски для детекции рамки: Otsu, CLAHE+порог, градиенты."""
    import cv2

    enhanced = clahe_gray(blur)
    _, otsu = cv2.threshold(enhanced, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    _, otsu_inv = cv2.threshold(enhanced, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU)
    masks = [otsu, otsu_inv]

    local = cv2.adaptiveThreshold(
        enhanced, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C, cv2.THRESH_BINARY, 21, -5
    )
    masks.append(local)
    masks.append(cv2.threshold(blur, 155, 255, cv2.THRESH_BINARY)[1])

    grad_y = cv2.Sobel(enhanced, cv2.CV_16S, 0, 1, ksize=3)
    grad_y = cv2.convertScaleAbs(grad_y)
    _, gy_mask = cv2.threshold(grad_y, 30, 255, cv2.THRESH_BINARY)
    masks.append(gy_mask)

    masks.append(cv2.Canny(enhanced, 40, 120))
    return masks


def sharpen(frame):
    import cv2
    import numpy as np

    kernel = np.array([[0, -1, 0], [-1, 5, -1], [0, -1, 0]], dtype=np.float32)
    return cv2.filter2D(frame, -1, kernel)


def upscale_min_height(frame, min_height: int = 96):
    import cv2

    height, width = frame.shape[:2]
    if height >= min_height:
        return frame
    scale = min_height / height
    return cv2.resize(
        frame,
        (max(1, int(width * scale)), min_height),
        interpolation=cv2.INTER_CUBIC,
    )


def add_ocr_border(
    crop,
    *,
    left: int = 14,
    right: int = 6,
    top: int = 4,
    bottom: int = 4,
):
    """Небольшой отступ вокруг кропа — первая буква не обрезается на краю кадра OCR."""
    import cv2

    if crop is None or crop.size == 0:
        return crop
    if left <= 0 and right <= 0 and top <= 0 and bottom <= 0:
        return crop
    return cv2.copyMakeBorder(
        crop,
        top,
        bottom,
        left,
        right,
        cv2.BORDER_REPLICATE,
    )


def prepare_plate_crop(crop, min_width: int = 480, min_height: int = 100):
    """Увеличить мелкий кроп перед OCR."""
    import cv2

    if crop is None or crop.size == 0:
        return crop
    scaled = upscale_min_height(crop, min_height)
    height, width = scaled.shape[:2]
    if width < min_width:
        scale = min_width / max(1, width)
        scaled = cv2.resize(
            scaled,
            (min_width, max(1, int(height * scale))),
            interpolation=cv2.INTER_CUBIC,
        )
    return add_ocr_border(scaled)


def _order_quad_points(points):
    """Упорядочить 4 угла: tl, tr, br, bl (Nomeroff: выравнивание перспективы)."""
    import numpy as np

    pts = np.array(points, dtype=np.float32).reshape(4, 2)
    s = pts.sum(axis=1)
    diff = np.diff(pts, axis=1).reshape(-1)
    tl = pts[np.argmin(s)]
    br = pts[np.argmax(s)]
    tr = pts[np.argmin(diff)]
    bl = pts[np.argmax(diff)]
    return np.array([tl, tr, br, bl], dtype=np.float32)


def warp_quad_plate(frame, contour, target_ratio: float = 4.6):
    """Перспективное выравнивание четырёхугольника номера перед OCR."""
    import cv2
    import numpy as np

    if frame is None or frame.size == 0 or contour is None:
        return None

    peri = cv2.arcLength(contour, True)
    approx = cv2.approxPolyDP(contour, 0.04 * peri, True)
    if len(approx) == 4:
        src = _order_quad_points(approx.reshape(4, 2))
    else:
        rect = cv2.minAreaRect(contour)
        src = _order_quad_points(cv2.boxPoints(rect))

    width_a = np.linalg.norm(src[0] - src[1])
    width_b = np.linalg.norm(src[2] - src[3])
    height_a = np.linalg.norm(src[0] - src[3])
    height_b = np.linalg.norm(src[1] - src[2])
    max_w = max(width_a, width_b)
    max_h = max(height_a, height_b)
    if max_w < 12 or max_h < 6:
        return None

    if max_w / max(max_h, 1) < target_ratio * 0.55:
        max_w, max_h = max_h, max_w
        src = np.array([src[0], src[3], src[2], src[1]], dtype=np.float32)

    # Чуть расширить левый край — перспектива иначе срезает первый символ.
    expand = max(2.0, max_w * 0.07)
    left_vec_top = src[0] - src[1]
    left_vec_bot = src[3] - src[2]
    for vec, idx in ((left_vec_top, 0), (left_vec_bot, 3)):
        norm = float(np.linalg.norm(vec))
        if norm > 1e-3:
            src[idx] = src[idx] + (vec / norm) * expand

    out_w = max(120, int(max_w))
    out_h = max(26, int(out_w / target_ratio))
    dst = np.array([[0, 0], [out_w - 1, 0], [out_w - 1, out_h - 1], [0, out_h - 1]], dtype=np.float32)
    matrix = cv2.getPerspectiveTransform(src, dst)
    warped = cv2.warpPerspective(frame, matrix, (out_w, out_h))
    if warped.size == 0:
        return None
    return warped


def ocr_variants(crop):
    """Нормализованные варианты кропа: как в Nomeroff — сначала выровнять зону, потом OCR."""
    import cv2

    if crop is None or crop.size == 0:
        return []
    base = upscale_min_height(crop)
    enhanced = sharpen(clahe_bgr(base))
    gray = cv2.cvtColor(enhanced, cv2.COLOR_BGR2GRAY)
    gray_bgr = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    return [base, enhanced, gray_bgr]
