"""The master dashboard: REST API, WebSocket push and the static single-page UI."""

from __future__ import annotations

import asyncio
import json
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import Body, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .firmware import MAX_IMAGE_BYTES, FirmwareStore
from .fleet import Fleet
from .models import Models
from .views import MAX_MODEL_BYTES, ViewStore

STATIC = Path(__file__).parent / "static"


def create_app(fleet: Fleet, views: ViewStore | None = None, models: Models | None = None,
               firmware: FirmwareStore | None = None, http_port: int = 8080) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        await fleet.start()
        yield
        await fleet.stop()

    app = FastAPI(title="Logger master", lifespan=lifespan)

    @app.get("/")
    async def index():
        return FileResponse(STATIC / "index.html", headers={"Cache-Control": "no-cache"})

    app.mount("/static", StaticFiles(directory=STATIC), name="static")

    # ------------------------------------------------------------ fleet

    @app.get("/api/fleet")
    async def fleet_list():
        return {"broker_connected": fleet.connected,
                "loggers": [fleet.summary(lg) for lg in fleet.loggers.values()]}

    @app.get("/api/loggers/{dev_id}")
    async def logger_detail(dev_id: str):
        d = fleet.detail(dev_id)
        if d is None:
            raise HTTPException(404, "unknown logger")
        return d

    @app.get("/api/features")
    async def features():
        return fleet.features.defs

    @app.post("/api/command")
    async def command(body: dict = Body(...)):
        try:
            return await fleet.command(body.get("selection") or {}, body["cmd"], body.get("target"),
                                       body.get("args"), float(body.get("timeout", 8)))
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e))

    @app.post("/api/loggers/{dev_id}/config")
    async def set_config(dev_id: str, body: dict = Body(...)):
        try:
            return await fleet.set_config(dev_id, body.get("changes"))
        except KeyError:
            raise HTTPException(404, "unknown logger")

    # ------------------------------------------------------------ registry

    @app.put("/api/registry/{dev_id}")
    async def registry_update(dev_id: str, body: dict = Body(...)):
        if not fleet.registry:
            raise HTTPException(400, "no registry configured")
        try:
            entry = fleet.registry.update(dev_id, body)
        except ValueError as e:
            raise HTTPException(400, str(e))
        lg = fleet.loggers.get(dev_id)
        if lg:
            fleet._emit({"type": "logger", "logger": fleet.summary(lg)})
        return entry

    # ------------------------------------------------------------ model catalogue

    @app.get("/api/models")
    async def model_list():
        return models.list() if models else []

    @app.get("/api/models/{model}")
    async def model_page(model: str):
        m = models.get(model) if models else None
        if not m:
            raise HTTPException(404, f"no catalogue page for model {model!r}")
        return m

    # ------------------------------------------------------------ orientation view (per logger)

    def _views() -> ViewStore:
        if not views:
            raise HTTPException(400, "no view storage configured")
        return views

    @app.get("/api/loggers/{dev_id}/view")
    async def view_get(dev_id: str):
        try:
            return _views().get(dev_id)
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.put("/api/loggers/{dev_id}/view")
    async def view_update(dev_id: str, body: dict = Body(...)):
        try:
            return _views().update(dev_id, body)
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.put("/api/loggers/{dev_id}/model")
    async def model_upload(dev_id: str, name: str, request: Request):
        data = await request.body()
        if len(data) > MAX_MODEL_BYTES:
            raise HTTPException(413, "model too large")
        try:
            return _views().save_model(dev_id, name, data)
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.get("/api/loggers/{dev_id}/model")
    async def model_get(dev_id: str):
        try:
            path = _views().model_path(dev_id)
        except ValueError as e:
            raise HTTPException(400, str(e))
        if not path:
            raise HTTPException(404, "no model")
        return FileResponse(path, media_type="application/octet-stream", headers={"Cache-Control": "no-cache"})

    @app.delete("/api/loggers/{dev_id}/model")
    async def model_delete(dev_id: str):
        try:
            return _views().delete_model(dev_id)
        except ValueError as e:
            raise HTTPException(400, str(e))

    # ------------------------------------------------------------ firmware (OTA updates)

    def _firmware() -> FirmwareStore:
        if not firmware:
            raise HTTPException(400, "no firmware storage configured")
        return firmware

    @app.get("/api/firmware")
    async def firmware_list():
        return _firmware().list()

    @app.put("/api/firmware")
    async def firmware_upload(model: str, version: str, request: Request, filename: str = "", notes: str = ""):
        data = await request.body()
        if len(data) > MAX_IMAGE_BYTES:
            raise HTTPException(413, f"image larger than the OTA slot ({MAX_IMAGE_BYTES} bytes)")
        try:
            return _firmware().save(model, version, data, filename, notes)
        except ValueError as e:
            raise HTTPException(400, str(e))

    @app.delete("/api/firmware/{model}/{version}")
    async def firmware_delete(model: str, version: str):
        try:
            if not _firmware().delete(model, version):
                raise HTTPException(404, "no such image")
        except ValueError as e:
            raise HTTPException(400, str(e))
        return {"deleted": f"{model}/{version}"}

    # What loggers download (the `update` command's path). A plain file with Content-Length: the
    # logger checks the size and SHA-256 before switching to it.
    @app.get("/firmware/{model}/{version}.bin")
    async def firmware_image(model: str, version: str):
        try:
            path = _firmware().path(model, version)
        except ValueError as e:
            raise HTTPException(400, str(e))
        if not path:
            raise HTTPException(404, "no such image")
        return FileResponse(path, media_type="application/octet-stream", headers={"Cache-Control": "no-store"})

    @app.post("/api/update")
    async def update(body: dict = Body(...)):
        try:
            image = _firmware().get(body.get("model", ""), body.get("version", ""))
        except ValueError as e:
            raise HTTPException(400, str(e))
        if not image:
            raise HTTPException(404, "no such firmware image on the master")
        try:
            return await fleet.update(body.get("selection") or {}, image, http_port, float(body.get("timeout", 8)))
        except ValueError as e:
            raise HTTPException(400, str(e))

    # ------------------------------------------------------------ time

    @app.get("/api/time")
    async def master_time():
        return {"utc_ms": int(time.time() * 1000)}

    @app.post("/api/time/loggers")
    async def set_logger_clocks(body: dict = Body(default={})):
        return await fleet.set_clocks(body.get("selection") or {"all": True})

    # ------------------------------------------------------------ live updates

    @app.websocket("/ws")
    async def ws(sock: WebSocket):
        await sock.accept()
        queue: asyncio.Queue = asyncio.Queue(maxsize=2000)

        def push(msg: dict) -> None:
            try:
                queue.put_nowait(msg)
            except asyncio.QueueFull:
                pass  # a slow client misses updates rather than slowing the fleet

        unsubscribe = fleet.listen(push)
        try:
            await sock.send_text(json.dumps({
                "type": "snapshot", "broker_connected": fleet.connected,
                "loggers": [fleet.summary(lg) for lg in fleet.loggers.values()],
            }))
            while True:
                try:
                    msg = await asyncio.wait_for(queue.get(), 20)
                except asyncio.TimeoutError:
                    msg = {"type": "ping"}  # notices closed sockets when the fleet is quiet
                await sock.send_text(json.dumps(msg))
        except (WebSocketDisconnect, RuntimeError):
            pass
        finally:
            unsubscribe()

    return app
