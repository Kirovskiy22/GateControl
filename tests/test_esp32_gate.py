import unittest
from unittest.mock import Mock

from services.esp32_gate import ESP32GateController


class FakeConfig:
    esp32_base_url = "http://127.0.0.1:10001"
    esp32_token = "test-token"
    esp32_timeout_sec = 1
    esp32_command_cooldown_sec = 0


class ESP32GateControllerTests(unittest.TestCase):
    def setUp(self):
        self.controller = ESP32GateController(FakeConfig())
        self.controller._request = Mock(return_value={"ok": True})

    def test_open_sends_start_pulse_endpoint(self):
        ok, message = self.controller.open_gate()
        self.assertTrue(ok)
        self.assertIn("положение ворот не подтверждено", message)
        self.controller._request.assert_called_once_with("open")

    def test_stop_uses_stop_endpoint(self):
        ok, _ = self.controller.stop_gate()
        self.assertTrue(ok)
        self.controller._request.assert_called_once_with("stop")

    def test_close_is_not_falsely_mapped_to_other_pin(self):
        ok, message = self.controller.close_gate()
        self.assertFalse(ok)
        self.assertIn("CLOSE не подключён", message)
        self.controller._request.assert_not_called()

    def test_status_never_claims_physical_position(self):
        status = self.controller.get_status()
        self.assertIsNone(status["state"])
        self.assertEqual(status["label"], "положение ворот неизвестно")


if __name__ == "__main__":
    unittest.main()
