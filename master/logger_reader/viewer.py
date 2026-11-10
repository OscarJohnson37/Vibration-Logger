"""The analysis viewer: a local web app over a folder of DLOG files (an SD card's /logs, or a copy).

    python -m logger_reader view E:/logs [--port 8090] [--cache DIR]

Lists the sessions in the folder; for one session it shows the whole recording (vibration level,
peaks, gravity per axis, battery, restarts, clipping, notes) and, zoomed into a few minutes, the
waveform, spectrum and spectrogram, with CSV export. Per-file summaries are cached on this PC
(analysis.SummaryCache), so a session is scanned once.
"""

from __future__ import annotations

import base64
import io
import threading
import webbrowser
from pathlib import Path

import numpy as np

from .analysis import (MAX_WINDOW_S, SessionView, SummaryCache, amplitude_spectrum, decimate_minmax, spectrogram,
                       window_stats)
from .reader import find_sessions

STATIC = Path(__file__).parent / "viewer_static"


class Viewer:
    def __init__(self, folder: str | Path, cache: SummaryCache):
        self.folder = Path(folder)
        self.cache = cache
        self.lock = threading.Lock()
        self.jobs: dict[str, dict] = {}
        self.views: dict[str, SessionView] = {}
        self.refresh()

    def refresh(self) -> None:
        self.sessions = {f"{s.name}~{s.device_id}": s for s in find_sessions(self.folder)}

    def view(self, key: str) -> SessionView:
        if key not in self.sessions:
            raise KeyError(key)
        v = self.views.get(key)
        if v is None:
            v = self.views[key] = SessionView(self.sessions[key], self.cache)
        return v

    def listing(self) -> list[dict]:
        out = []
        for key, s in self.sessions.items():
            paths = [p for b in s.boots for p in s.files[b]]
            size, bad = 0, 0
            for p in paths:
                try:
                    size += p.stat().st_size
                except OSError:  # damaged card: the file can't even be looked at
                    bad += 1
            out.append({"key": key, "name": s.name, "logger": s.logger, "device_id": s.device_id,
                        "boots": s.boots, "files": len(paths) + len(s.unreadable), "bytes": size,
                        "unreadable": bad + len(s.unreadable),
                        "cached": sum(self.cache.has(p) for p in paths),
                        "job": self.jobs.get(key)})
        return out

    def prepare(self, key: str) -> dict:
        v = self.view(key)
        with self.lock:
            job = self.jobs.get(key)
            if job and job["state"] == "running":
                return job
            job = self.jobs[key] = {"state": "running", "done": 0, "total": len(v.missing()), "error": None}

        def progress(done: int, total: int, _path: str) -> None:
            job["done"], job["total"] = done, total

        def run() -> None:
            try:
                v.prepare(progress)
                job["state"] = "ready"
            except Exception as e:  # shown in the page
                job["state"], job["error"] = "failed", f"{type(e).__name__}: {e}"

        threading.Thread(target=run, daemon=True).start()
        return job


def create_app(viewer: Viewer):
    from fastapi import FastAPI, HTTPException, Query
    from fastapi.responses import FileResponse, StreamingResponse
    from fastapi.staticfiles import StaticFiles

    app = FastAPI(title="Logger data viewer")

    def view_ready(key: str) -> SessionView:
        try:
            v = viewer.view(key)
        except KeyError:
            raise HTTPException(404, "no such session")
        if not v.ready:
            if v.missing():
                raise HTTPException(409, "session not prepared yet")
            v.ensure()
        return v

    @app.get("/")
    async def index():
        return FileResponse(STATIC / "index.html", headers={"Cache-Control": "no-cache"})

    app.mount("/static", StaticFiles(directory=STATIC), name="static")

    @app.get("/api/sessions")
    def sessions(refresh: bool = False):
        if refresh:
            viewer.refresh()
        return {"folder": str(viewer.folder), "sessions": viewer.listing()}

    @app.post("/api/sessions/{key}/prepare")
    def prepare(key: str):
        try:
            return viewer.prepare(key)
        except KeyError:
            raise HTTPException(404, "no such session")

    @app.get("/api/sessions/{key}")
    def session(key: str):
        try:
            v = viewer.view(key)
        except KeyError:
            raise HTTPException(404, "no such session")
        job = viewer.jobs.get(key)
        if not v.ready and not v.missing():
            v.ensure()
        out = {"key": key, "name": v.session.name, "logger": v.session.logger, "ready": v.ready, "job": job,
               "missing": len(v.missing()) if not v.ready else 0, "max_window_s": MAX_WINDOW_S}
        if v.ready:
            out.update(v.info())
        return out

    @app.get("/api/sessions/{key}/overview")
    def overview(key: str, t0: float, t1: float, bins: int = 1000):
        return view_ready(key).overview(t0, t1, bins)

    @app.get("/api/sessions/{key}/window")
    def window(key: str, t0: float, t1: float, points: int = 1500):
        v = view_ready(key)
        try:
            w = v.window(t0, t1)
        except ValueError as e:
            raise HTTPException(400, str(e))
        out = {"t0": t0, "t1": t1, "stats": window_stats(w), "channels": {}, "unit": w.unit, "rate_hz": w.rate_hz}
        for ci, name in enumerate(w.channels):
            t, lo, hi = decimate_minmax(w.t, w.values[:, ci], max(100, min(points, 4000)))
            out["channels"][name] = {"t": t, "min": lo, "max": hi}
        if w.clipped.any():
            out["clipped_t"] = w.t[w.clipped][:2000].tolist()
        return out

    @app.get("/api/sessions/{key}/spectrum")
    def spectrum(key: str, t0: float, t1: float, nfft: int = 8192):
        v = view_ready(key)
        try:
            w = v.window(t0, t1)
        except ValueError as e:
            raise HTTPException(400, str(e))
        if len(w.t) < 64:
            raise HTTPException(400, "not enough data in this window")
        out = {"unit": w.unit, "channels": {}}
        for ci, name in enumerate(w.channels):
            f, amp = amplitude_spectrum(w.values[:, ci], w.rate_hz, max(256, min(nfft, 65536)))
            out["freq"] = f.tolist()
            out["channels"][name] = amp.tolist()
        return out

    @app.get("/api/sessions/{key}/spectrogram")
    def spectro(key: str, t0: float, t1: float, channel: str, nfft: int = 512):
        v = view_ready(key)
        try:
            w = v.window(t0, t1)
        except ValueError as e:
            raise HTTPException(400, str(e))
        if channel not in w.channels:
            raise HTTPException(400, f"no channel {channel!r}")
        sg = spectrogram(w.values[:, w.channels.index(channel)], w.rate_hz, max(64, min(nfft, 4096)))
        db = sg["db"]
        if not len(db):
            raise HTTPException(400, "not enough data in this window")
        lo, hi = np.percentile(db, 5), np.percentile(db, 99.9)
        img = np.clip((db - lo) / max(hi - lo, 1e-9) * 255, 0, 255).astype(np.uint8)  # (cols, freqs)
        return {"cols": int(img.shape[0]), "rows": int(img.shape[1]), "db_min": float(lo), "db_max": float(hi),
                "freq_max": sg["freq_max"], "t0": float(w.t[0]), "t1": float(w.t[-1]),
                "data": base64.b64encode(img.tobytes()).decode()}

    @app.get("/api/sessions/{key}/export")
    def export(key: str, t0: float, t1: float, utc: bool = Query(True)):
        v = view_ready(key)
        try:
            w = v.window(t0, t1)
        except ValueError as e:
            raise HTTPException(400, str(e))

        def rows():
            import pandas as pd

            step = 200_000
            for i in range(0, max(len(w.t), 1), step):
                t = w.t[i:i + step]
                df = pd.DataFrame(w.values[i:i + step], columns=[f"{c}_{w.unit}" for c in w.channels])
                df.insert(0, "time_utc" if utc else "t_s",
                          pd.to_datetime(t, unit="s", utc=True).strftime("%Y-%m-%dT%H:%M:%S.%fZ") if utc else t)
                df["clipped"] = w.clipped[i:i + step].astype(int)
                buf = io.StringIO()
                df.to_csv(buf, index=False, header=(i == 0), float_format="%.6f")
                yield buf.getvalue()

        name = f"{v.session.name}_{v.session.logger}_{int(t0)}-{int(t1)}.csv"
        return StreamingResponse(rows(), media_type="text/csv",
                                 headers={"Content-Disposition": f'attachment; filename="{name}"'})

    return app


def serve(folder: str | Path, port: int = 8090, cache_dir: str | Path | None = None, open_browser: bool = True) -> None:
    import uvicorn

    viewer = Viewer(folder, SummaryCache(cache_dir))
    url = f"http://localhost:{port}"
    print(f"data viewer: {url}  ({len(viewer.sessions)} sessions in {folder}; cache {viewer.cache.folder})")
    if open_browser:
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()
    uvicorn.run(create_app(viewer), host="127.0.0.1", port=port, log_level="warning")
