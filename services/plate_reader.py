from __future__ import annotations

import logging
import re

from dataclasses import dataclass

from services.plate import (
    _PARTIAL_BODY_RE,
    _SINGLE_LETTER_RE,
    is_valid_ru_plate,
    normalize_plate,
    ocr_confidence,
    repair_ocr_plate,
)
from services.plate_finder import find_plate_crops
from services.preprocess import ocr_variants, pad_box, prepare_plate_crop

logger = logging.getLogger("gatecontrol.plate_reader")


@dataclass(frozen=True)
class PlateRead:
    text: str
    confidence: float
    source: str = "alpr"


class PlateReader:
    """YOLO на кадре + OCR по кропам (fast-plate-ocr), запасной поиск пластины через OpenCV."""

    def __init__(
        self,
        detector_model: str = "yolo-v9-t-384-license-plate-end2end",
        ocr_model: str = "cct-xs-v2-global-model",
        *,
        easyocr_enabled: bool = False,
        cv_fallback: bool = True,
    ):
        self.detector_model = detector_model
        self.ocr_model = ocr_model
        self.easyocr_enabled = easyocr_enabled
        self.cv_fallback = cv_fallback
        self._alpr = None
        self._ocr = None
        self._easy = None
        self._easy_failed = False

    def load(self) -> None:
        from fast_alpr import ALPR
        from fast_alpr.default_ocr import DefaultOCR

        self._alpr = ALPR(
            detector_model=self.detector_model,
            ocr_model=self.ocr_model,
        )
        self._ocr = DefaultOCR(hub_ocr_model=self.ocr_model, device="cpu")
        if self.easyocr_enabled:
            self._load_easyocr()

    def _load_easyocr(self) -> None:
        if self._easy_failed:
            return
        try:
            import easyocr

            self._easy = easyocr.Reader(["ru", "en"], gpu=False, verbose=False)
        except Exception:
            self._easy = None
            self._easy_failed = True

    def read(self, frame, *, roi_limited: bool = False) -> list[PlateRead]:
        try:
            alpr_reads, yolo_crops = self._read_alpr(frame)
        except Exception:
            logger.exception("Ошибка FastALPR")
            alpr_reads, yolo_crops = [], []

        has_valid = any(repair_ocr_plate(item.text) for item in alpr_reads)
        extra: list[PlateRead] = []

        if self.cv_fallback and not has_valid:
            cv_crops: list = []
            try:
                extra.extend(self._read_alpr_crops(self._fallback_windows(frame, roi_limited=roi_limited)))
            except Exception:
                logger.exception("Ошибка ALPR по окнам кадра")
            try:
                cv_crops = find_plate_crops(frame, roi_limited=roi_limited)
                extra.extend(self._read_alpr_crops(cv_crops[:3]))
            except Exception:
                logger.exception("Ошибка ALPR по кропам OpenCV")
            try:
                extra.extend(self._read_crops_ocr(cv_crops[:3]))
            except Exception:
                logger.exception("Ошибка OCR по кропам")
        elif yolo_crops:
            try:
                extra.extend(self._read_crops_ocr(yolo_crops))
            except Exception:
                logger.exception("Ошибка OCR по кропам")

        if self.easyocr_enabled and not has_valid:
            try:
                all_crops = find_plate_crops(frame, roi_limited=roi_limited) + self._fallback_windows(
                    frame, roi_limited=roi_limited
                )
                extra.extend(self._read_easyocr(all_crops))
            except Exception:
                logger.exception("Ошибка EasyOCR")

        return self._rank(self._merge_fragments(alpr_reads + extra))

    def _merge_fragments(self, combined: list[PlateRead]) -> list[PlateRead]:
        bodies: list[PlateRead] = []
        partials: list[tuple[str, float, str]] = []
        letters: list[tuple[str, float]] = []
        regions: list[tuple[str, float]] = []
        rest: list[PlateRead] = []
        for item in combined:
            norm = normalize_plate(item.text)
            if re.fullmatch(r"\d{2,3}", norm):
                regions.append((norm, item.confidence))
                continue
            if _SINGLE_LETTER_RE.fullmatch(norm):
                letters.append((norm, item.confidence))
                continue
            partial = _PARTIAL_BODY_RE.fullmatch(norm)
            if partial:
                partials.append((partial.group(1), item.confidence, item.source))
                continue
            repaired = repair_ocr_plate(item.text)
            if repaired and is_valid_ru_plate(repaired):
                if len(repaired) > 6:
                    rest.append(
                        PlateRead(text=repaired, confidence=item.confidence, source=item.source)
                    )
                else:
                    bodies.append(
                        PlateRead(text=repaired, confidence=item.confidence, source=item.source)
                    )
            else:
                rest.append(item)

        regions.sort(key=lambda pair: pair[1], reverse=True)
        bodies.sort(key=lambda item: item.confidence)
        letters.sort(key=lambda pair: pair[1], reverse=True)
        used_regions: set[str] = set()
        used_bodies: set[str] = set()
        used_partials: set[str] = set()
        used_letters: set[str] = set()

        merged = list(rest)
        for body in bodies:
            if body.text in used_bodies:
                continue
            merged_text = body.text
            merged_conf = body.confidence
            for reg, rconf in regions:
                if reg in used_regions:
                    continue
                candidate = repair_ocr_plate(body.text + reg)
                if candidate and is_valid_ru_plate(candidate) and len(candidate) > 6:
                    merged_text = candidate
                    merged_conf = (body.confidence + rconf) / 2.0
                    used_regions.add(reg)
                    used_bodies.add(body.text)
                    break
            merged.append(
                PlateRead(text=merged_text, confidence=merged_conf, source=body.source)
            )

        for partial, pconf, source in partials:
            if partial in used_partials:
                continue
            for letter, lconf in letters:
                if letter in used_letters:
                    continue
                candidate = repair_ocr_plate(letter + partial)
                if not candidate or not is_valid_ru_plate(candidate):
                    continue
                merged_text = candidate
                merged_conf = (pconf + lconf) / 2.0
                if len(candidate) <= 6:
                    for reg, rconf in regions:
                        if reg in used_regions:
                            continue
                        full = repair_ocr_plate(merged_text + reg)
                        if full and is_valid_ru_plate(full) and len(full) > 6:
                            merged_text = full
                            merged_conf = (merged_conf + rconf) / 2.0
                            used_regions.add(reg)
                            break
                merged.append(
                    PlateRead(text=merged_text, confidence=merged_conf, source=source)
                )
                used_partials.add(partial)
                used_letters.add(letter)
                break
        return merged

    def _rank(self, combined: list[PlateRead]) -> list[PlateRead]:
        ranked: list[PlateRead] = []
        rest: list[PlateRead] = []
        seen: set[str] = set()
        for item in combined:
            repaired = repair_ocr_plate(item.text)
            text = repaired or normalize_plate(item.text)
            if not text or len(text) < 4 or text in seen:
                continue
            seen.add(text)
            read = PlateRead(text=text, confidence=item.confidence, source=item.source)
            if repaired and is_valid_ru_plate(repaired):
                ranked.append(read)
            elif repaired:
                ranked.append(read)
            else:
                rest.append(read)

        def sort_key(item: PlateRead) -> tuple[int, int, float]:
            valid = is_valid_ru_plate(item.text)
            has_region = valid and len(item.text) > 6
            return (int(valid), int(has_region), item.confidence)

        ranked.sort(key=sort_key, reverse=True)
        rest.sort(key=lambda item: item.confidence, reverse=True)
        return ranked + rest

    def _read_alpr(self, frame) -> tuple[list[PlateRead], list]:
        if self._alpr is None:
            raise RuntimeError("Модель распознавания ещё не загружена")

        found: list[PlateRead] = []
        crops: list = []
        for item in self._alpr.predict(frame):
            crop = self._crop_detection(frame, getattr(item, "detection", None))
            if crop is not None and len(crops) < 3:
                crops.append(crop)
            ocr = getattr(item, "ocr", None)
            if ocr is None or not getattr(ocr, "text", None):
                continue
            found.append(
                PlateRead(
                    text=str(ocr.text),
                    confidence=ocr_confidence(getattr(ocr, "confidence", 0.0)),
                    source="alpr",
                )
            )
        return found, crops

    def _read_alpr_crops(self, crops: list) -> list[PlateRead]:
        if self._alpr is None or not crops:
            return []
        found: list[PlateRead] = []
        for crop in crops:
            prepared = prepare_plate_crop(crop)
            for item in self._alpr.predict(prepared):
                ocr = getattr(item, "ocr", None)
                if ocr is None or not getattr(ocr, "text", None):
                    continue
                found.append(
                    PlateRead(
                        text=str(ocr.text),
                        confidence=ocr_confidence(getattr(ocr, "confidence", 0.0)) * 0.95,
                        source="alpr-crop",
                    )
                )
        return found

    def _read_crops_ocr(self, crops: list) -> list[PlateRead]:
        if self._ocr is None or not crops:
            return []
        found: list[PlateRead] = []
        for crop in crops:
            prepared = prepare_plate_crop(crop)
            for image in ocr_variants(prepared):
                result = self._ocr.predict(image)
                if result is None or not getattr(result, "text", None):
                    continue
                text = str(result.text).strip()
                if not text:
                    continue
                conf = ocr_confidence(getattr(result, "confidence", 0.0))
                found.append(PlateRead(text=text, confidence=conf, source="ocr-crop"))
        return found

    def _read_easyocr(self, crops: list) -> list[PlateRead]:
        if self._easy is None:
            return []
        found: list[PlateRead] = []
        for image in self._iter_variants(crops):
            for item in self._easy.readtext(image):
                if len(item) < 3:
                    continue
                text = str(item[1])
                conf = ocr_confidence(item[2])
                if text.strip():
                    found.append(PlateRead(text=text, confidence=conf, source="easyocr"))
        return found

    def _iter_variants(self, crops: list):
        for crop in crops:
            yield from ocr_variants(crop)

    def _crop_detection(self, frame, detection):
        if detection is None:
            return None
        box = getattr(detection, "bounding_box", None)
        if box is None:
            return None
        try:
            x1, y1, x2, y2 = float(box.x1), float(box.y1), float(box.x2), float(box.y2)
        except AttributeError:
            return None
        height, width = frame.shape[:2]
        left, top, right, bottom = pad_box(
            x1, y1, x2, y2, width, height, pad=0.18, pad_left=0.30
        )
        crop = frame[top:bottom, left:right]
        if crop.size == 0:
            return None
        return crop

    def _fallback_windows(self, frame, *, roi_limited: bool = False):
        """Окна поиска: верхний центр + центр (камера над воротами, номер на стенде)."""
        height, width = frame.shape[:2]
        if height < 120 or width < 120:
            return [prepare_plate_crop(frame)]
        if roi_limited:
            specs = [
                (0.00, 1.00, 0.00, 1.00),
                (0.05, 0.50, 0.15, 0.85),
                (0.35, 0.95, 0.20, 0.80),
                (0.20, 0.55, 0.30, 0.70),
            ]
        else:
            specs = [
                (0.18, 0.58, 0.20, 0.80),  # широкий центр — номер в руках
                (0.10, 0.36, 0.28, 0.72),  # верхний центр — стенд / шлагбаум
                (0.14, 0.40, 0.35, 0.65),  # узкий верхний центр
                (0.22, 0.52, 0.22, 0.78),  # средняя полоса
            ]
        windows = []
        seen: set[tuple[int, int]] = set()
        for y0, y1, x0, x1 in specs:
            crop = frame[int(height * y0) : int(height * y1), int(width * x0) : int(width * x1)]
            if crop.size == 0:
                continue
            key = crop.shape[1], crop.shape[0]
            if key in seen:
                continue
            seen.add(key)
            windows.append(prepare_plate_crop(crop))
        return windows
