import asyncio
import threading

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

        self._thread.start()
        self._ready.wait()

    def _worker_loop(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)

        self._gate = SNMPGate()
        self._ready.set()

        try:
            self._loop.run_forever()
        finally:
            self._loop.close()

    def _run(self, coro):
        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        return future.result()

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
        future.result()

        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=3)

        if self._thread.is_alive():
            raise RuntimeError("Не удалось корректно остановить SNMP Worker")
