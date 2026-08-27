from datetime import datetime

from config import Config
from services.snmp_worker import SNMPWorker


class GateController:
    """Связка GUI и SNMP через фоновый worker."""

    def __init__(self, ui):
        self.ui = ui
        self.cfg = Config()
        self.worker = SNMPWorker()
        self._pending = False
        self._do1_state: int | None = None
        self.ui.after(100, self._sync_initial_state)

    def log(self, text: str) -> None:
        time = datetime.now().strftime("%H:%M:%S")
        self.ui.log.insert("end", f"[{time}] {text}\n")
        self.ui.log.see("end")

    def _set_buttons_state(self, state: str) -> None:
        self.ui.open_btn.configure(state=state)
        self.ui.close_btn.configure(state=state)
        self.ui.refresh_btn.configure(state=state)

    def _set_status(self, state: int | None) -> None:
        if state == 0:
            self.ui.status_label.configure(text="🟢 Шлагбаум поднят")
        elif state == 1:
            self.ui.status_label.configure(text="🔴 Шлагбаум опущен")
        elif state == 2:
            self.ui.status_label.configure(text="⚡ Импульс отправлен")
        else:
            self.ui.status_label.configure(text="⚪ Состояние неизвестно")

    def _finish_command(self) -> None:
        self._pending = False
        self._set_buttons_state("normal")

    def _release_buttons(self) -> None:
        self._set_buttons_state("normal")

    def _sync_initial_state(self) -> None:
        def on_success(state):
            self._do1_state = state
            self.ui.after(0, lambda: self._set_status(state))
            self.ui.after(0, lambda: self.log(f"Начальное состояние DO1: {state}"))

        def on_error(exc):
            self.ui.after(0, lambda: self.log(f"Не удалось получить состояние: {exc}"))

        self.worker.submit_get_state(on_success=on_success, on_error=on_error)

    def _begin_command(self) -> bool:
        if self._pending:
            self.log("Команда уже выполняется, подождите")
            return False

        self._pending = True
        self._set_buttons_state("disabled")
        return True

    def _should_skip_hold_command(self, target_state: int) -> bool:
        if self.cfg.do1_mode != "hold":
            return False

        if self._do1_state == target_state:
            return True

        return False

    def _dispatch(self, submit_fn, target_state: int | None, log_text: str) -> None:
        if not self._begin_command():
            return

        if target_state is not None and self._should_skip_hold_command(target_state):
            self.log(f"Пропуск: DO1 уже в состоянии {target_state}")
            self._finish_command()
            return

        def on_success(state):
            if target_state is not None and self.cfg.do1_mode == "hold":
                self._do1_state = target_state
            elif self.cfg.do1_mode == "pulse":
                self._do1_state = None

            self.ui.after(0, lambda: self._set_status(state if state is not None else target_state))

        def on_error(exc):
            self.ui.after(0, lambda: self.log(f"Ошибка: {exc}"))

        if submit_fn(on_success=on_success, on_error=on_error):
            self.log(log_text)
            self._pending = False
            self._release_buttons()
        else:
            self.log("SNMP worker занят")
            self._finish_command()

    def open_gate(self) -> None:
        self._dispatch(
            self.worker.submit_open,
            0,
            "Команда: поднять (SET 0)" if self.cfg.do1_mode == "hold" else "Команда: импульс (SET 2)",
        )

    def close_gate(self) -> None:
        self._dispatch(
            self.worker.submit_close,
            1,
            "Команда: опустить (SET 1)" if self.cfg.do1_mode == "hold" else "Команда: импульс (SET 2)",
        )

    def refresh(self) -> None:
        if not self._begin_command():
            return

        def on_success(state):
            self._do1_state = state
            self.ui.after(0, lambda: self._set_status(state))
            self.ui.after(0, lambda: self.log(f"Состояние DO1: {state}"))

        def on_error(exc):
            self.ui.after(0, lambda: self.log(f"Ошибка: {exc}"))

        if self.worker.submit_get_state(on_success=on_success, on_error=on_error):
            self.log("Запрос состояния отправлен")
            self._pending = False
            self._release_buttons()
        else:
            self.log("SNMP worker занят")
            self._finish_command()

    def shutdown(self) -> None:
        try:
            self.worker.shutdown()
            self.log("SNMP worker остановлен")
        except Exception as e:
            self.log(f"Ошибка остановки: {e}")
