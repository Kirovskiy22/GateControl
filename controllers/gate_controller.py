import threading
from datetime import datetime

from config import Config
from services.esp32_gate import ESP32GateController


class GateController:
    """Desktop controls for ESP32 wired to the single START input."""

    def __init__(self, ui):
        self.ui = ui
        self.cfg = Config()
        self.controller = ESP32GateController(self.cfg)
        self._pending = False
        self.ui.after(100, self.refresh)

    def log(self, text: str) -> None:
        time = datetime.now().strftime("%H:%M:%S")
        self.ui.log.insert("end", f"[{time}] {text}\n")
        self.ui.log.see("end")

    def _set_buttons_state(self, state: str) -> None:
        self.ui.open_btn.configure(state=state)
        self.ui.close_btn.configure(state=state)
        self.ui.refresh_btn.configure(state=state)

    def _set_status(self) -> None:
        self.ui.status_label.configure(text="⚪ Положение ворот неизвестно (нет датчика)")

    def _dispatch(self, action, label: str) -> None:
        if self._pending:
            self.log("Команда уже выполняется, подождите")
            return
        self._pending = True
        self._set_buttons_state("disabled")

        def run():
            ok, message = action()
            def finish():
                self._pending = False
                self._set_buttons_state("normal")
                self._set_status()
                self.log(message or label if ok else f"Ошибка: {message}")
            self.ui.after(0, finish)

        threading.Thread(target=run, name="ESP32Command", daemon=True).start()

    def open_gate(self) -> None:
        self._dispatch(self.controller.open_gate, "START / импульс")

    def close_gate(self) -> None:
        self.log("Отдельный CLOSE не подключён; используйте START для переключения движения")

    def stop_gate(self) -> None:
        self._dispatch(self.controller.stop_gate, "STOP / снять выходы ESP32")

    def refresh(self) -> None:
        self._dispatch(self.controller.refresh, "Статус ESP32 обновлён")

    def shutdown(self) -> None:
        self.log("Управление ESP32 завершено")
