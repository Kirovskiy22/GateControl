import asyncio
import threading
import time
from typing import Callable

from services.snmp_gate import SNMPGate


class SNMPWorker:
    """Фоновый поток с постоянным asyncio loop для SNMP."""

    def __init__(self):
        self._thread = threading.Thread(
            target=self._worker_loop,
            name="SNMPWorker",
            daemon=True,
        )
        self._ready = threading.Event()
        self._loop = None
        self._gate = None
        self._lock = None
        self._busy = threading.Event()
        self._busy.set()
        self._last_command_at = 0.0

        self._thread.start()
        self._ready.wait()

    def _worker_loop(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._lock = asyncio.Lock()

        self._gate = SNMPGate()
        self._ready.set()

        try:
            self._loop.run_forever()
        finally:
            self._loop.close()

    def _cooldown_sec(self) -> float:
        if self._gate.cfg.do1_mode == "pulse":
            return self._gate.cfg.pulse_cooldown_sec
        return self._gate.cfg.command_cooldown_sec

    def _wait_cooldown(self) -> None:
        elapsed = time.monotonic() - self._last_command_at
        remaining = self._cooldown_sec() - elapsed
        if remaining > 0:
            time.sleep(remaining)

    def _run(self, coro):
        if not self._busy.wait(timeout=5):
            raise RuntimeError("SNMP worker занят")

        self._busy.clear()
        try:
            self._wait_cooldown()

            async def _locked():
                async with self._lock:
                    return await coro

            future = asyncio.run_coroutine_threadsafe(_locked(), self._loop)
            result = future.result(timeout=10)
            self._last_command_at = time.monotonic()
            return result
        finally:
            self._busy.set()

    def submit(
        self,
        coro_factory: Callable[[], object],
        *,
        on_success: Callable[[object], None] | None = None,
        on_error: Callable[[Exception], None] | None = None,
    ) -> bool:
        """Запускает SNMP-команду без блокировки вызывающего потока."""

        if not self._busy.is_set():
            return False

        def _worker():
            try:
                result = self._run(coro_factory())
                if on_success:
                    on_success(result)
            except Exception as exc:
                if on_error:
                    on_error(exc)

        threading.Thread(target=_worker, name="SNMPCommand", daemon=True).start()
        return True

    def submit_open(
        self,
        *,
        on_success: Callable[[object], None] | None = None,
        on_error: Callable[[Exception], None] | None = None,
    ) -> bool:
        return self.submit(self._gate.open_gate, on_success=on_success, on_error=on_error)

    def submit_close(
        self,
        *,
        on_success: Callable[[object], None] | None = None,
        on_error: Callable[[Exception], None] | None = None,
    ) -> bool:
        return self.submit(self._gate.close_gate, on_success=on_success, on_error=on_error)

    def submit_get_state(
        self,
        *,
        on_success: Callable[[object], None] | None = None,
        on_error: Callable[[Exception], None] | None = None,
    ) -> bool:
        return self.submit(self._gate.get_state, on_success=on_success, on_error=on_error)

    def open_gate(self) -> None:
        return self._run(self._gate.open_gate())

    def close_gate(self) -> None:
        return self._run(self._gate.close_gate())

    def pulse_gate(self) -> None:
        return self._run(self._gate.pulse_gate())

    def get_state(self) -> int:
        return self._run(self._gate.get_state())

    async def _shutdown_gate(self) -> None:
        self._gate.close()

    def shutdown(self) -> None:
        if self._loop is None or self._loop.is_closed():
            return

        future = asyncio.run_coroutine_threadsafe(
            self._shutdown_gate(),
            self._loop,
        )
        future.result(timeout=5)

        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=3)

        if self._thread.is_alive():
            raise RuntimeError("Не удалось корректно остановить SNMP Worker")
