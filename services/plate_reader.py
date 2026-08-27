from __future__ import annotations

from dataclasses import dataclass

from services.plate import ocr_confidence


@dataclass(frozen=True)
class PlateRead:
    text: str
    confidence: float


class PlateReader:
    """Обёртка над FastALPR: детектор номера + OCR."""

    def __init__(
        self,
        detector_model: str = "yolo-v9-t-384-license-plate-end2end",
        ocr_model: str = "cct-xs-v2-global-model",
    ):
        self.detector_model = detector_model
        self.ocr_model = ocr_model
        self._alpr = None

    def load(self) -> None:
        from fast_alpr import ALPR

        self._alpr = ALPR(
            detector_model=self.detector_model,
            ocr_model=self.ocr_model,
        )

    def read(self, frame) -> list[PlateRead]:
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
        found.sort(key=lambda item: item.confidence, reverse=True)
        return found
