import unittest

from services.plate import (
    OpenCooldown,
    PlateAccessPolicy,
    format_plate,
    is_valid_ru_plate,
    normalize_plate,
    ocr_confidence,
)


class NormalizePlateTests(unittest.TestCase):
    def test_latin_lookalikes_and_spaces(self):
        self.assertEqual(normalize_plate("a 123 bc 77"), "А123ВС77")

    def test_strips_rus_suffix(self):
        self.assertEqual(normalize_plate("А123ВС77RUS"), "А123ВС77")

    def test_valid_format(self):
        self.assertTrue(is_valid_ru_plate("А123ВС77"))
        self.assertTrue(is_valid_ru_plate("А123ВС777"))
        self.assertFalse(is_valid_ru_plate("123ABC"))
        self.assertEqual(format_plate("А123ВС77"), "А 123 ВС 77")


class PolicyTests(unittest.TestCase):
    def test_any_valid_plate_opens_without_whitelist(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=False,
            require_valid_format=True,
            min_confidence=0.6,
            open_on_detect=True,
        )
        decision = policy.evaluate("A123BC77", 0.91)
        self.assertTrue(decision.open_gate)
        self.assertEqual(decision.plate, "А123ВС77")

    def test_whitelist_only_rejects_unknown(self):
        policy = PlateAccessPolicy(
            ["А123ВС77"],
            whitelist_only=True,
            require_valid_format=True,
            min_confidence=0.6,
            open_on_detect=True,
        )
        unknown = policy.evaluate("В456ОР199", 0.95)
        known = policy.evaluate("A123BC77", 0.88)
        self.assertFalse(unknown.open_gate)
        self.assertTrue(known.open_gate)

    def test_low_confidence_and_empty_whitelist(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=True,
            require_valid_format=True,
            min_confidence=0.7,
            open_on_detect=True,
        )
        low = policy.evaluate("А123ВС77", 0.2)
        empty = policy.evaluate("А123ВС77", 0.9)
        self.assertFalse(low.open_gate)
        self.assertFalse(empty.open_gate)
        self.assertIn("белый список пуст", empty.reason)


class HelperTests(unittest.TestCase):
    def test_ocr_confidence_list_and_percent(self):
        self.assertAlmostEqual(ocr_confidence([0.9, 0.7]), 0.8)
        self.assertAlmostEqual(ocr_confidence(85), 0.85)

    def test_cooldown_blocks_then_releases(self):
        cooldown = OpenCooldown(10)
        now = 100.0
        self.assertFalse(cooldown.blocked(now)[0])
        cooldown.mark("А123ВС77", now)
        blocked, reason = cooldown.blocked(now + 3)
        self.assertTrue(blocked)
        self.assertIn("пауза", reason)
        self.assertFalse(cooldown.blocked(now + 11)[0])


if __name__ == "__main__":
    unittest.main()
