from __future__ import annotations

from dataclasses import dataclass

from services.plate import ocr_confidence, repair_ocr_plate


@dataclass(frozen=True)
class PlateRead:
    text: str
    confidence: float


class PlateReader:
    """Детектор автомобильных номеров + запасной OCR текста на кадре."""

    def __init__(
        self,
        detector_model: str = "yolo-v9-t-384-license-plate-end2end",
        ocr_model: str = "cct-xs-v2-global-model",
    ):
        self.detector_model = detector_model
        self.ocr_model = ocr_model
        self._alpr = None
        self._easy = None
        self._easy_failed = False

    def load(self) -> None:
        from fast_alpr import ALPR

        self._alpr = ALPR(
            detector_model=self.detector_model,
            ocr_model=self.ocr_model,
        )
        self._load_easyocr()

    def _load_easyocr(self) -> None:
        try:
            import easyocr

            self._easy = easyocr.Reader(["ru", "en"], gpu=False, verbose=False)
        except Exception:
            self._easy = None
            self._easy_failed = True

    def read(self, frame) -> list[PlateRead]:
        combined = self._read_alpr(frame) + self._read_text(frame)
        ranked: list[PlateRead] = []
        rest: list[PlateRead] = []
        seen: set[str] = set()
        for item in combined:
            repaired = repair_ocr_plate(item.text)
            text = repaired or item.text
            key = text
            if key in seen:
                continue
            seen.add(key)
            read = PlateRead(text=text, confidence=item.confidence)
            if repaired:
                ranked.append(read)
            else:
                rest.append(read)
        ranked.sort(key=lambda item: item.confidence, reverse=True)
        rest.sort(key=lambda item: item.confidence, reverse=True)
        return ranked + rest

    def _read_alpr(self, frame) -> list[PlateRead]:
        if self._alpr is None:
            raise RuntimeError("Модель распознавания ещё не загружена")

        found: list[PlateRead] = []
        for item in self._alpr.predict(frame):
            ocr = getattr(item, "ocr", None)
            if ocr is None or not getattr(ocr, "text", None):
                continue
            found.append(
                PlateRead(
                    text=str(ocr.text),
                    confidence=ocr_confidence(getattr(ocr, "confidence", 0.0)),
                )
            )
        return found

    def _read_text(self, frame) -> list[PlateRead]:
        if self._easy is None:
            return []

        found: list[PlateRead] = []
        for image in self._text_windows(frame):
            results = self._easy.readtext(image)
            for item in results:
                if len(item) < 3:
                    continue
                text = str(item[1])
                conf = ocr_confidence(item[2])
                if text.strip():
                    found.append(PlateRead(text=text, confidence=conf))
        return found

    def _text_windows(self, frame):
        yield frame
        height, width = frame.shape[:2]
        if height < 240 or width < 240:
            return
        yield frame[int(height * 0.15) : int(height * 0.85), int(width * 0.18) : int(width * 0.82)]
