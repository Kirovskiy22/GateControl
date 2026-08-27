import threading
from datetime import datetime

from config import Config
from services.snmp_worker import SNMPWorker


def state_label(state: int | None) -> str:
    if state == 0:
        return "поднят"
    if state == 1:
        return "опущен"
    return "неизвестно"


class WebGateController:
    """Управление шлагбаумом через SNMP для веб-API."""

    def __init__(self, worker: SNMPWorker):
        self.cfg = Config()
        self.worker = worker
        self._pending = False
        self._lock = threading.Lock()
        self._do1_state: int | None = None

    def log(self, text: str) -> str:
        time = datetime.now().strftime("%H:%M:%S")
        return f"[{time}] {text}"

    def get_status(self) -> dict:
        return {"state": self._do1_state, "label": state_label(self._do1_state)}

    def sync_initial_state(self) -> None:
        self._run_async(self.worker.submit_get_state, None)

    def _begin_command(self) -> bool:
        with self._lock:
            if self._pending:
                return False
            self._pending = True
            return True

    def _finish_command(self) -> None:
        with self._lock:
            self._pending = False

    def _should_skip_hold_command(self, target_state: int) -> bool:
        if self.cfg.do1_mode != "hold":
            return False
        return self._do1_state == target_state

    def _run_async(
        self,
        submit_fn,
        target_state: int | None,
    ) -> tuple[bool, str | None]:
        if not self._begin_command():
            return False, "Команда уже выполняется, подождите"

        if target_state is not None and self._should_skip_hold_command(target_state):
            self._finish_command()
            return True, f"Пропуск: DO1 уже в состоянии {target_state}"

        done = threading.Event()
        result: dict = {"error": None}

        def on_success(state):
            if target_state is None:
                self._do1_state = state
            elif self.cfg.do1_mode == "hold":
                self._do1_state = target_state
            elif self.cfg.do1_mode == "pulse":
                self._do1_state = None
            done.set()

        def on_error(exc):
            result["error"] = exc
            done.set()

        if not submit_fn(on_success=on_success, on_error=on_error):
            self._finish_command()
            return False, "SNMP worker занят"

        if not done.wait(timeout=15):
            self._finish_command()
            return False, "Таймаут SNMP-команды"

        self._finish_command()

        if result["error"]:
            return False, str(result["error"])

        return True, None

    def open_gate(self) -> tuple[bool, str | None]:
        log_text = (
            "Команда: поднять (SET 0)"
            if self.cfg.do1_mode == "hold"
            else "Команда: импульс (SET 2)"
        )
        ok, err = self._run_async(self.worker.submit_open, 0)
        if not ok:
            return False, err
        if err:
            return True, err
        return True, log_text

    def close_gate(self) -> tuple[bool, str | None]:
        log_text = (
            "Команда: опустить (SET 1)"
            if self.cfg.do1_mode == "hold"
            else "Команда: импульс (SET 2)"
        )
        ok, err = self._run_async(self.worker.submit_close, 1)
        if not ok:
            return False, err
        if err:
            return True, err
        return True, log_text

    def refresh(self) -> tuple[bool, str | None]:
        ok, err = self._run_async(self.worker.submit_get_state, None)
        if not ok:
            return False, err
        if err:
            return True, err
        return True, f"Состояние DO1: {self._do1_state}"
