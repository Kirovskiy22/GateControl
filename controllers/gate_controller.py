from datetime import datetime

from services.snmp_worker import SNMPWorker


class GateController:
    """Связка GUI и SNMP через фоновый worker."""

    def __init__(self, ui):
        self.ui = ui
        self.worker = SNMPWorker()

    def log(self, text: str) -> None:
        time = datetime.now().strftime("%H:%M:%S")
        self.ui.log.insert("end", f"[{time}] {text}\n")
        self.ui.log.see("end")

    def _set_status(self, state: int | None) -> None:
        if state == 0:
            self.ui.status_label.configure(text="🟢 Шлагбаум поднят")
        elif state == 1:
            self.ui.status_label.configure(text="🔴 Шлагбаум опущен")
        else:
            self.ui.status_label.configure(text="⚪ Состояние неизвестно")

    def open_gate(self) -> None:
        try:
            self.worker.open_gate()
            self._set_status(0)
            self.log("Команда: поднять (SET 0)")
        except Exception as e:
            self.log(f"Ошибка: {e}")

    def close_gate(self) -> None:
        try:
            self.worker.close_gate()
            self._set_status(1)
            self.log("Команда: опустить (SET 1)")
        except Exception as e:
            self.log(f"Ошибка: {e}")

    def refresh(self) -> None:
        try:
            state = self.worker.get_state()
            self._set_status(state)
            self.log(f"Состояние DO1: {state}")
        except Exception as e:
            self.log(f"Ошибка: {e}")

    def shutdown(self) -> None:
        try:
            self.worker.shutdown()
            self.log("SNMP worker остановлен")
        except Exception as e:
            self.log(f"Ошибка остановки: {e}")
