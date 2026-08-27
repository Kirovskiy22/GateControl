from __future__ import annotations

import re
from dataclasses import dataclass

CYRILLIC_PLATE_LETTERS = "АВЕКМНОРСТУХ"
LATIN_TO_CYRILLIC = str.maketrans(
    {
        "A": "А",
        "B": "В",
        "E": "Е",
        "K": "К",
        "M": "М",
        "H": "Н",
        "O": "О",
        "P": "Р",
        "C": "С",
        "T": "Т",
        "Y": "У",
        "X": "Х",
    }
)
RU_PLATE_RE = re.compile(
    rf"^[{CYRILLIC_PLATE_LETTERS}]\d{{3}}[{CYRILLIC_PLATE_LETTERS}]{{2}}\d{{2,3}}$"
)


def normalize_plate(raw: str | None) -> str:
    if not raw:
        return ""
    text = raw.upper().replace(" ", "").replace("-", "").replace(".", "")
    text = re.sub(r"RUS$", "", text)
    text = re.sub(r"[^A-ZА-Я0-9]", "", text)
    return text.translate(LATIN_TO_CYRILLIC)


def is_valid_ru_plate(plate: str) -> bool:
    return bool(RU_PLATE_RE.fullmatch(plate))


def format_plate(plate: str) -> str:
    if not is_valid_ru_plate(plate):
        return plate
    letters_tail = 2
    region_len = len(plate) - 6
    return f"{plate[0]} {plate[1:4]} {plate[4:6]} {plate[6:6 + region_len]}"


def ocr_confidence(value: object) -> float:
    if value is None:
        return 0.0
    if isinstance(value, (list, tuple)):
        nums = [float(item) for item in value if item is not None]
        if not nums:
            return 0.0
        value = sum(nums) / len(nums)
    conf = float(value)
    if conf > 1.0:
        conf = conf / 100.0
    return max(0.0, min(conf, 1.0))


@dataclass(frozen=True)
class PlateDecision:
    plate: str
    confidence: float
    open_gate: bool
    reason: str
    kind: str


class PlateAccessPolicy:
    def __init__(
        self,
        allowed_plates: list[str] | None,
        *,
        whitelist_only: bool,
        require_valid_format: bool,
        min_confidence: float,
        open_on_detect: bool,
    ):
        self.allowed = {
            normalize_plate(item) for item in (allowed_plates or []) if item and item.strip()
        }
        self.whitelist_only = whitelist_only
        self.require_valid_format = require_valid_format
        self.min_confidence = min_confidence
        self.open_on_detect = open_on_detect

    def evaluate(self, raw_text: str, confidence: float) -> PlateDecision:
        plate = normalize_plate(raw_text)
        conf = ocr_confidence(confidence)

        if not plate:
            return PlateDecision("", conf, False, "пустой номер", "warn")

        if conf < self.min_confidence:
            return PlateDecision(
                plate,
                conf,
                False,
                f"низкая уверенность {conf:.0%} (порог {self.min_confidence:.0%})",
                "warn",
            )

        if self.require_valid_format and not is_valid_ru_plate(plate):
            return PlateDecision(
                plate,
                conf,
                False,
                "номер не похож на российский ГРЗ",
                "warn",
            )

        if not self.open_on_detect:
            return PlateDecision(
                plate,
                conf,
                False,
                "автооткрытие выключено",
                "info",
            )

        in_whitelist = plate in self.allowed
        if self.whitelist_only:
            if not self.allowed:
                return PlateDecision(
                    plate,
                    conf,
                    False,
                    "белый список пуст — шлагбаум не открывается",
                    "info",
                )
            if not in_whitelist:
                return PlateDecision(
                    plate,
                    conf,
                    False,
                    "номера нет в белом списке",
                    "warn",
                )

        reason = "номер в белом списке" if in_whitelist else "номер распознан"
        return PlateDecision(plate, conf, True, reason, "success")


class OpenCooldown:
    def __init__(self, seconds: float):
        self.seconds = max(0.0, float(seconds))
        self._until = 0.0
        self._plate = ""

    def remaining(self, now: float) -> float:
        return max(0.0, self._until - now)

    def blocked(self, now: float) -> tuple[bool, str]:
        left = self.remaining(now)
        if left > 0:
            shown = format_plate(self._plate) if self._plate else "открытия"
            return True, f"пауза {left:.0f} с после {shown}"
        return False, ""

    def mark(self, plate: str, now: float) -> None:
        self._plate = plate
        self._until = now + self.seconds
