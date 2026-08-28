from contextlib import asynccontextmanager
from pathlib import Path
import threading

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from config import Config
from services.anpr_worker import AnprWorker
from services.snmp_worker import SNMPWorker
from web.controllers.web_gate_controller import WebGateController

STATIC_DIR = Path(__file__).parent / "static"
API_VERSION = 2


def _snmp_poll_loop(controller: WebGateController, interval_sec: float, stop: threading.Event) -> None:
    while not stop.wait(max(5.0, interval_sec)):
        try:
            controller.refresh()
        except Exception:
            pass


@asynccontextmanager
async def lifespan(app: FastAPI):
    cfg = Config()
    worker = SNMPWorker()
    controller = WebGateController(worker)
    anpr = AnprWorker(
        open_gate=controller.open_gate,
        close_gate=controller.close_gate,
        cfg=cfg,
    )
    snmp_stop = threading.Event()
    app.state.controller = controller
    app.state.worker = worker
    app.state.anpr = anpr
    app.state.snmp_stop = snmp_stop
    threading.Thread(
        target=controller.sync_initial_state,
        name="InitialStateSync",
        daemon=True,
    ).start()
    threading.Thread(
        target=_snmp_poll_loop,
        args=(controller, cfg.snmp_poll_interval_sec, snmp_stop),
        name="SNMPPoll",
        daemon=True,
    ).start()
    anpr.start()
    yield
    snmp_stop.set()
    anpr.stop()
    worker.shutdown()


app = FastAPI(title="Gate Control", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def full_status(extra: dict | None = None) -> dict:
    payload = app.state.controller.get_status()
    payload["anpr"] = app.state.anpr.get_status()
    payload["api_version"] = API_VERSION
    if extra:
        payload.update(extra)
    return payload


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/status")
async def api_status():
    return full_status()


@app.get("/api/anpr/snapshot")
async def api_anpr_snapshot():
    jpeg = app.state.anpr.last_jpeg()
    if not jpeg:
        raise HTTPException(status_code=404, detail="Нет кадра с камеры")
    return Response(
        content=jpeg,
        media_type="image/jpeg",
        headers={"Cache-Control": "no-store"},
    )


class StreamRequest(BaseModel):
    url: str = Field(min_length=1)
    flip: bool | None = None


@app.post("/api/anpr/stream")
async def api_anpr_stream(body: StreamRequest):
    ok, message = app.state.anpr.set_stream(body.url, flip=body.flip)
    if not ok:
        raise HTTPException(status_code=400, detail=message)
    return full_status({"ok": True, "message": message})


@app.post("/api/open")
async def api_open():
    ok, message = app.state.controller.open_gate()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


@app.post("/api/close")
async def api_close():
    ok, message = app.state.controller.close_gate()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


@app.post("/api/refresh")
async def api_refresh():
    ok, message = app.state.controller.refresh()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


def main() -> None:
    cfg = Config()
    import uvicorn

    uvicorn.run(
        "web.app:app",
        host=cfg.web_host,
        port=cfg.web_port,
        reload=False,
    )


if __name__ == "__main__":
    main()
