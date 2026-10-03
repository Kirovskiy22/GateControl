from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from config import Config
from services.anpr_worker import AnprWorker
from services.esp32_gate import ESP32GateController

STATIC_DIR = Path(__file__).parent / "static"
API_VERSION = 3


@asynccontextmanager
async def lifespan(app: FastAPI):
    cfg = Config()
    controller = ESP32GateController(cfg)
    anpr = AnprWorker(open_gate=controller.open_gate, cfg=cfg)
    app.state.controller = controller
    app.state.anpr = anpr
    anpr.start()
    yield
    anpr.stop()


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
def api_status():
    ok, message = app.state.controller.refresh()
    payload = full_status()
    payload["controller_message"] = message
    payload["controller_ok"] = ok
    return payload


@app.get("/api/anpr/snapshot")
async def api_anpr_snapshot():
    jpeg = app.state.anpr.last_jpeg()
    if not jpeg:
        raise HTTPException(status_code=404, detail="Нет кадра с камеры")
    return Response(content=jpeg, media_type="image/jpeg", headers={"Cache-Control": "no-store"})


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
def api_open():
    ok, message = app.state.controller.open_gate()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


@app.post("/api/stop")
def api_stop():
    ok, message = app.state.controller.stop_gate()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


@app.post("/api/close")
def api_close():
    raise HTTPException(
        status_code=409,
        detail="Отдельная команда CLOSE не поддерживается: используется один переключающий вход START. Отправьте импульс START вручную.",
    )


@app.post("/api/refresh")
def api_refresh():
    ok, message = app.state.controller.refresh()
    if not ok:
        raise HTTPException(status_code=503, detail=message)
    return full_status({"ok": True, "message": message})


def main() -> None:
    cfg = Config()
    import uvicorn
    uvicorn.run("web.app:app", host=cfg.web_host, port=cfg.web_port, reload=False)


if __name__ == "__main__":
    main()
