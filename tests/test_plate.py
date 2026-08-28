import unittest

from services.plate import (
    OpenCooldown,
    PlateAccessPolicy,
    extract_plate_candidates,
    format_plate,
    is_valid_ru_plate,
    normalize_plate,
    ocr_confidence,
    repair_ocr_plate,
)


class NormalizePlateTests(unittest.TestCase):
    def test_latin_lookalikes_and_spaces(self):
        self.assertEqual(normalize_plate("a 123 bc 77"), "А123ВС77")

    def test_strips_rus_suffix(self):
        self.assertEqual(normalize_plate("А123ВС77RUS"), "А123ВС77")

    def test_extracts_plate_from_noise(self):
        self.assertEqual(extract_plate_candidates("номер A182MH"), ["А182МН"])

    def test_repairs_mirrored_ocr(self):
        self.assertEqual(repair_ocr_plate("C108EC154"), "С108ЕС154")
        self.assertEqual(repair_ocr_plate("1108EC154"), "С108ЕС154")
        self.assertEqual(repair_ocr_plate("A18ZMII"), "А182МН")
        self.assertEqual(repair_ocr_plate("A182MH"), "А182МН")
        self.assertEqual(repair_ocr_plate("108EC154"), "")
        self.assertEqual(repair_ocr_plate("STOP"), "")

    def test_valid_format(self):
        self.assertTrue(is_valid_ru_plate("А123ВС77"))
        self.assertTrue(is_valid_ru_plate("А123ВС777"))
        self.assertTrue(is_valid_ru_plate("А182МН"))
        self.assertFalse(is_valid_ru_plate("123ABC"))
        self.assertEqual(format_plate("А123ВС77"), "А 123 ВС 77")
        self.assertEqual(format_plate("А182МН"), "А 182 МН")


class PolicyTests(unittest.TestCase):
    def test_any_valid_plate_opens_without_whitelist(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=False,
            require_valid_format=True,
            min_confidence=0.6,
            open_min_confidence=0.6,
            require_region=False,
            open_on_detect=True,
        )
        decision = policy.evaluate("A123BC77", 0.91)
        self.assertTrue(decision.open_gate)
        self.assertEqual(decision.plate, "А123ВС77")
        short = policy.evaluate("А182МН", 0.8)
        self.assertTrue(short.open_gate)
        self.assertEqual(short.plate, "А182МН")

    def test_require_region_blocks_short_plate(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=False,
            require_valid_format=True,
            min_confidence=0.55,
            open_min_confidence=0.6,
            require_region=True,
            open_on_detect=True,
        )
        short = policy.evaluate("А182МН", 0.8)
        self.assertFalse(short.open_gate)
        self.assertIn("региона", short.reason)
        full = policy.evaluate("C108EC154", 0.75)
        self.assertTrue(full.open_gate)
        self.assertEqual(full.plate, "С108ЕС154")

    def test_open_requires_higher_confidence(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=False,
            require_valid_format=True,
            min_confidence=0.55,
            open_min_confidence=0.7,
            require_region=False,
            open_on_detect=True,
        )
        low_open = policy.evaluate("A123BC77", 0.65)
        self.assertFalse(low_open.open_gate)
        self.assertIn("открытия", low_open.reason)
        high_open = policy.evaluate("A123BC77", 0.75)
        self.assertTrue(high_open.open_gate)

    def test_unrepaired_garbage_rejected(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=False,
            require_valid_format=True,
            min_confidence=0.55,
            open_min_confidence=0.6,
            require_region=False,
            open_on_detect=True,
        )
        garbage = policy.evaluate("CG0B5154", 0.7)
        self.assertFalse(garbage.open_gate)

    def test_whitelist_only_rejects_unknown(self):
        allowed = [
            "А123ВВ77",
            "В456СС99",
            "Е789КК78",
            "К321ММ178",
            "О654РР50",
            "Т987ХХ90",
            "Н246АА142",
            "С135УУ42",
            "М864ВВ193",
            "Р753СС123",
            "У426КК66",
            "Х537ММ166",
            "А918ОО154",
            "В273ТТ54",
            "Е649РР164",
            "К815ХХ64",
            "Р392АА102",
            "С704ВВ702",
            "Т158КК22",
            "У861ММ122",
            "С108ЕС154",
        ]
        policy = PlateAccessPolicy(
            allowed,
            whitelist_only=True,
            require_valid_format=True,
            min_confidence=0.55,
            open_min_confidence=0.6,
            require_region=True,
            open_on_detect=True,
        )
        test_plate = policy.evaluate("C108EC154", 0.75)
        self.assertTrue(test_plate.open_gate)
        self.assertEqual(test_plate.plate, "С108ЕС154")
        self.assertIn("белом списке", test_plate.reason)
        unknown = policy.evaluate("В456ОР199", 0.95)
        self.assertFalse(unknown.open_gate)
        self.assertIn("белом списке", unknown.reason)
        known = policy.evaluate("A123BB77", 0.88)
        self.assertTrue(known.open_gate)
        self.assertEqual(known.plate, "А123ВВ77")

    def test_low_confidence_and_empty_whitelist(self):
        policy = PlateAccessPolicy(
            [],
            whitelist_only=True,
            require_valid_format=True,
            min_confidence=0.7,
            open_min_confidence=0.7,
            require_region=False,
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


class AutoCloseTrackerTests(unittest.TestCase):
    def test_waits_close_after_sec_without_plate(self):
        from services.plate import AutoCloseTracker

        tracker = AutoCloseTracker(8.0)
        now = 100.0
        self.assertFalse(tracker.should_close(now, 0.0))
        self.assertFalse(tracker.should_close(now + 5, 0.0))
        self.assertTrue(tracker.should_close(now + 8, 0.0))

    def test_resets_when_plate_returns(self):
        from services.plate import AutoCloseTracker

        tracker = AutoCloseTracker(5.0)
        now = 0.0
        tracker.should_close(now, 0.0)
        tracker.should_close(now + 4, 0.0)
        tracker.observe_plate()
        self.assertFalse(tracker.should_close(now + 4.5, 0.0))
        self.assertTrue(tracker.should_close(now + 9.5, 0.0))

    def test_pauses_during_open_cooldown(self):
        from services.plate import AutoCloseTracker

        tracker = AutoCloseTracker(3.0)
        now = 10.0
        tracker.should_close(now, 0.0)
        self.assertFalse(tracker.should_close(now + 5, 4.0))
        self.assertFalse(tracker.should_close(now + 5.5, 0.0))
        self.assertTrue(tracker.should_close(now + 8.5, 0.0))

    def test_closes_once_per_empty_streak(self):
        from services.plate import AutoCloseTracker

        tracker = AutoCloseTracker(2.0)
        now = 0.0
        self.assertFalse(tracker.should_close(now, 0.0))
        self.assertTrue(tracker.should_close(now + 2, 0.0))
        tracker.mark_closed()
        self.assertFalse(tracker.should_close(now + 10, 0.0))

    def test_disabled_never_closes(self):
        from services.plate import AutoCloseTracker

        tracker = AutoCloseTracker(1.0, enabled=False)
        self.assertFalse(tracker.should_close(0.0, 0.0))
        self.assertFalse(tracker.should_close(100.0, 0.0))


class AnprAutoCloseTests(unittest.TestCase):
    def test_auto_close_after_empty_frames(self):
        from unittest.mock import MagicMock, patch

        from services.anpr_worker import AnprWorker

        closes: list[tuple[bool, str | None]] = []

        class FakeCfg:
            anpr_allowed_plates = []
            anpr_whitelist_only = False
            anpr_require_valid_format = False
            anpr_min_confidence = 0.55
            anpr_open_min_confidence = 0.6
            anpr_require_region = False
            anpr_open_on_detect = True
            anpr_open_cooldown_sec = 0.0
            anpr_auto_close = True
            anpr_close_after_sec = 5.0
            anpr_easyocr_enabled = False
            anpr_cv_fallback = False
            anpr_roi_enabled = False
            anpr_roi = {"x0": 0.0, "y0": 0.0, "x1": 1.0, "y1": 1.0}

        worker = AnprWorker(
            open_gate=lambda: (True, None),
            close_gate=lambda: closes.append((True, "SET 1")) or (True, "SET 1"),
            cfg=FakeCfg(),
        )
        worker._reader = MagicMock()
        worker._reader.read.return_value = []

        with patch("services.anpr_worker.time.monotonic", side_effect=[100.0, 106.0]):
            worker._handle_frame(object(), 100.0)
            worker._handle_frame(object(), 106.0)

        self.assertEqual(len(closes), 1)

    def test_plate_in_frame_resets_close_timer(self):
        from unittest.mock import MagicMock, patch

        from services.anpr_worker import AnprWorker
        from services.plate_reader import PlateRead

        closes: list[bool] = []

        class FakeCfg:
            anpr_allowed_plates = []
            anpr_whitelist_only = False
            anpr_require_valid_format = False
            anpr_min_confidence = 0.55
            anpr_open_min_confidence = 0.6
            anpr_require_region = False
            anpr_open_on_detect = True
            anpr_open_cooldown_sec = 0.0
            anpr_auto_close = True
            anpr_close_after_sec = 5.0
            anpr_easyocr_enabled = False
            anpr_cv_fallback = False
            anpr_roi_enabled = False
            anpr_roi = {"x0": 0.0, "y0": 0.0, "x1": 1.0, "y1": 1.0}

        worker = AnprWorker(
            open_gate=lambda: (True, None),
            close_gate=lambda: closes.append(True) or (True, None),
            cfg=FakeCfg(),
        )
        worker._reader = MagicMock()
        worker._reader.read.return_value = [PlateRead("A123BC77", 0.9, "test")]

        with patch("services.anpr_worker.time.monotonic", side_effect=[100.0, 104.0, 108.0, 109.0, 112.0]):
            worker._handle_frame(object(), 100.0)
            worker._reader.read.return_value = []
            worker._handle_frame(object(), 104.0)
            worker._reader.read.return_value = [PlateRead("A123BC77", 0.9, "test")]
            worker._handle_frame(object(), 108.0)
            worker._reader.read.return_value = []
            worker._handle_frame(object(), 109.0)
            worker._handle_frame(object(), 112.0)

        self.assertEqual(closes, [])


class RoiTests(unittest.TestCase):
    def test_clamp_swapped_and_out_of_bounds(self):
        from services.roi import clamp_roi

        roi = clamp_roi({"x0": 0.9, "y0": 0.8, "x1": 0.1, "y1": 0.2})
        self.assertLess(roi["x0"], roi["x1"])
        self.assertLess(roi["y0"], roi["y1"])
        self.assertGreaterEqual(roi["x0"], 0.0)
        self.assertLessEqual(roi["x1"], 1.0)

    def test_crop_roi_extracts_center_band(self):
        import numpy as np

        from services.roi import crop_roi, roi_pixel_box

        frame = np.zeros((100, 200, 3), dtype=np.uint8)
        frame[10:50, 40:160] = 255
        roi = {"x0": 0.2, "y0": 0.1, "x1": 0.8, "y1": 0.5}
        left, top, right, bottom = roi_pixel_box(frame.shape, roi)
        cropped = crop_roi(frame, roi)
        self.assertEqual((left, top, right, bottom), (40, 10, 160, 50))
        self.assertEqual(cropped.shape, (40, 120, 3))
        self.assertTrue(cropped.all())

    def test_crop_roi_rejects_empty(self):
        import numpy as np

        from services.roi import crop_roi

        frame = np.zeros((10, 10, 3), dtype=np.uint8)
        cropped = crop_roi(frame, {"x0": 0.0, "y0": 0.0, "x1": 0.01, "y1": 0.01})
        self.assertGreater(cropped.size, 0)


class PreprocessTests(unittest.TestCase):
    def test_pad_box_expands_and_clamps(self):
        from services.preprocess import pad_box

        left, top, right, bottom = pad_box(10, 10, 20, 20, 100, 80, pad=0.5)
        self.assertEqual((left, top, right, bottom), (5, 5, 25, 25))

    def test_pad_box_extra_left_margin(self):
        from services.preprocess import pad_box

        left, top, right, bottom = pad_box(
            40, 10, 140, 40, 200, 100, pad=0.10, pad_left=0.28
        )
        self.assertLess(left, 40 - int(100 * 0.10))
        self.assertGreaterEqual(left, 0)
        self.assertGreater(right, 140)

    def test_prepare_plate_crop_adds_left_border(self):
        import numpy as np

        from services.preprocess import add_ocr_border, prepare_plate_crop

        crop = np.zeros((40, 120, 3), dtype=np.uint8)
        prepared = prepare_plate_crop(crop, min_width=120, min_height=40)
        bordered = add_ocr_border(crop, left=14, right=6, top=4, bottom=4)
        self.assertGreater(prepared.shape[1], crop.shape[1])
        self.assertGreater(bordered.shape[1], crop.shape[1])
        self.assertGreater(bordered.shape[0], crop.shape[0])


class StreamUrlTests(unittest.TestCase):
    def test_rejects_non_stream_url(self):
        from services.anpr_worker import AnprWorker

        worker = AnprWorker(open_gate=lambda: (True, None))
        ok, message = worker.set_stream("camera.local/stream")
        self.assertFalse(ok)
        self.assertIn("rtsp", message.lower())


class StreamTuneTests(unittest.TestCase):
    def test_light_stream_short_interval(self):
        from services.stream_probe import tune_stream

        profile = tune_stream(640, 480, 15)
        self.assertEqual(profile.tier, "light")
        self.assertLessEqual(profile.process_interval_sec, 1.0)
        self.assertGreaterEqual(profile.preview_interval_sec, 2.0)

    def test_heavy_stream_longer_interval(self):
        from services.stream_probe import tune_stream

        profile = tune_stream(2592, 1920, 25)
        self.assertEqual(profile.tier, "heavy")
        self.assertGreaterEqual(profile.process_interval_sec, 2.0)
        self.assertLessEqual(profile.resize_width, 1280)


class MotionTests(unittest.TestCase):
    def test_identical_frames_zero_motion(self):
        import numpy as np

        from services.motion import frame_motion_score, has_motion

        gray = np.full((60, 160), 128, dtype=np.uint8)
        self.assertAlmostEqual(frame_motion_score(gray, gray), 0.0)
        self.assertFalse(has_motion(gray, gray, 0.01))

    def test_changed_frame_detects_motion(self):
        import numpy as np

        from services.motion import frame_motion_score, has_motion

        prev = np.zeros((60, 160), dtype=np.uint8)
        curr = np.full((60, 160), 255, dtype=np.uint8)
        self.assertGreater(frame_motion_score(prev, curr), 0.9)
        self.assertTrue(has_motion(prev, curr, 0.02))

    def test_downscale_gray_from_bgr(self):
        import cv2
        import numpy as np

        from services.motion import downscale_gray

        frame = np.zeros((480, 640, 3), dtype=np.uint8)
        cv2.rectangle(frame, (100, 100), (200, 150), (255, 255, 255), -1)
        gray = downscale_gray(frame, width=160)
        self.assertEqual(gray.shape[1], 160)
        self.assertEqual(len(gray.shape), 2)


class PlateFinderTests(unittest.TestCase):
    def test_finds_bright_rectangle(self):
        import cv2
        import numpy as np

        from services.plate_finder import find_plate_crops

        frame = np.zeros((480, 640, 3), dtype=np.uint8)
        cv2.rectangle(frame, (180, 200), (460, 260), (240, 240, 240), -1)
        crops = find_plate_crops(frame)
        self.assertGreaterEqual(len(crops), 1)

    def test_finds_upper_center_plate(self):
        import cv2
        import numpy as np

        from services.plate_finder import find_plate_crops

        frame = np.zeros((480, 640, 3), dtype=np.uint8)
        cv2.rectangle(frame, (250, 90), (390, 125), (235, 235, 235), -1)
        crops = find_plate_crops(frame)
        self.assertGreaterEqual(len(crops), 1)


class PreprocessWarpTests(unittest.TestCase):
    def test_warp_quad_from_rectangle(self):
        import cv2
        import numpy as np

        from services.preprocess import warp_quad_plate

        frame = np.zeros((120, 320, 3), dtype=np.uint8)
        cv2.rectangle(frame, (40, 40), (280, 80), (255, 255, 255), -1)
        contour = np.array([[[40, 40]], [[280, 40]], [[280, 80]], [[40, 80]]])
        warped = warp_quad_plate(frame, contour)
        self.assertIsNotNone(warped)
        self.assertGreater(warped.shape[1], warped.shape[0] * 2)

    def test_gray_plate_masks_nonempty(self):
        import cv2
        import numpy as np

        from services.preprocess import gray_plate_masks

        gray = np.zeros((80, 200), dtype=np.uint8)
        cv2.rectangle(gray, (30, 25), (170, 55), 230, -1)
        blur = cv2.GaussianBlur(gray, (5, 5), 0)
        masks = gray_plate_masks(gray, blur)
        self.assertGreaterEqual(len(masks), 4)
        self.assertTrue(any(mask.any() for mask in masks))


class PlateReaderMergeTests(unittest.TestCase):
    def test_merge_body_and_region_once(self):
        from services.plate_reader import PlateRead, PlateReader

        reader = PlateReader()
        combined = [
            PlateRead("C108EC", 0.7, "alpr"),
            PlateRead("A182MH", 0.8, "alpr"),
            PlateRead("154", 0.65, "ocr-crop"),
        ]
        merged = reader._merge_fragments(combined)
        texts = {item.text for item in merged}
        self.assertIn("С108ЕС154", texts)
        self.assertNotIn("А182МН154", texts)

    def test_merge_letter_partial_and_region(self):
        from services.plate_reader import PlateRead, PlateReader

        reader = PlateReader()
        combined = [
            PlateRead("C", 0.72, "alpr"),
            PlateRead("108EC", 0.68, "ocr-crop"),
            PlateRead("154", 0.66, "ocr-crop"),
        ]
        merged = reader._merge_fragments(combined)
        texts = {item.text for item in merged}
        self.assertIn("С108ЕС154", texts)

    def test_rank_prefers_full_plate_with_region(self):
        from services.plate_reader import PlateRead, PlateReader

        reader = PlateReader()
        combined = [
            PlateRead("А182МН", 0.9, "alpr"),
            PlateRead("С108ЕС154", 0.7, "alpr"),
        ]
        ranked = reader._rank(combined)
        self.assertEqual(ranked[0].text, "С108ЕС154")


class PlateReaderFallbackTests(unittest.TestCase):
    def _load_test_frame(self):
        import os

        import cv2
        import numpy as np

        for path in ("check_frame.jpg", "tests/fixtures/plate_crop.jpg"):
            if not os.path.exists(path):
                continue
            image = cv2.imread(path)
            if image is None:
                continue
            if path.endswith("plate_crop.jpg"):
                frame = np.zeros((474, 640, 3), dtype=np.uint8)
                h, w = image.shape[:2]
                frame[85 : 85 + h, 128 : 128 + w] = image
                return frame
            return image
        return None

    def test_reads_russian_plate_from_saved_frame(self):
        import cv2

        from services.plate_reader import PlateReader

        frame = self._load_test_frame()
        if frame is None:
            self.skipTest("нет check_frame.jpg и tests/fixtures/plate_crop.jpg")
        reader = PlateReader(cv_fallback=True)
        reader.load()
        reads = reader.read(frame)
        texts = [item.text for item in reads]
        self.assertTrue(
            any("108" in text and ("ЕС" in text or "EC" in text.upper()) for text in texts),
            f"ожидали С108ЕС154, получили {texts}",
        )


if __name__ == "__main__":
    unittest.main()
