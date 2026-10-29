"""Per-logger orientation view settings and 3D models, kept on the master so every browser (a
phone included) shows each logger the same way.

    registry/views/<device_id>.json   { zero, axis_map, model_align, model }
    registry/views/<device_id>.<stl|obj|glb>

zero        gravity in sensor axes when the logger was in its reference position
axis_map    3x3 signed permutation: shown axes = axis_map x sensor axes (which sensor axis drives
            which model axis, like tools/accel-viewer's "Data axes")
model_align quaternion [x, y, z, w] turning the model relative to the axes
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path

MODEL_TYPES = ("stl", "obj", "glb")
MAX_MODEL_BYTES = 30 * 1024 * 1024
_ID = re.compile(r"^[0-9A-Za-z_-]{1,40}$")


def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _valid(key: str, v) -> bool:
    if key == "zero":
        return v is None or (isinstance(v, list) and len(v) == 3 and all(_is_number(x) for x in v))
    if key == "axis_map":
        if not (isinstance(v, list) and len(v) == 3 and all(isinstance(r, list) and len(r) == 3 for r in v)):
            return False
        cols = [[r[j] for r in v] for j in range(3)]
        return all(sorted(map(abs, r)) == [0, 0, 1] for r in v) and all(sorted(map(abs, c)) == [0, 0, 1] for c in cols)
    if key == "model_align":
        return isinstance(v, list) and len(v) == 4 and all(_is_number(x) for x in v)
    return False


class ViewStore:
    def __init__(self, folder: Path):
        self.folder = Path(folder)

    def _path(self, dev_id: str, suffix: str) -> Path:
        if not _ID.match(dev_id):
            raise ValueError("bad logger id")
        return self.folder / f"{dev_id}.{suffix}"

    def get(self, dev_id: str) -> dict:
        try:
            return json.loads(self._path(dev_id, "json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _write(self, dev_id: str, view: dict) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        path = self._path(dev_id, "json")
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(view, indent=1), encoding="utf-8")
        os.replace(tmp, path)

    def update(self, dev_id: str, changes: dict) -> dict:
        """Apply {zero, axis_map, model_align} (any subset); raises ValueError on bad values."""
        view = self.get(dev_id)
        for key, v in changes.items():
            if key not in ("zero", "axis_map", "model_align") or not _valid(key, v):
                raise ValueError(f"invalid {key!r}")
            if v is None:
                view.pop(key, None)
            else:
                view[key] = v
        self._write(dev_id, view)
        return view

    def save_model(self, dev_id: str, name: str, data: bytes) -> dict:
        ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
        if ext not in MODEL_TYPES:
            raise ValueError("model must be an STL, OBJ or GLB file")
        if not data or len(data) > MAX_MODEL_BYTES:
            raise ValueError(f"model must be 1 byte to {MAX_MODEL_BYTES // (1024 * 1024)} MB")
        self.delete_model(dev_id)
        self.folder.mkdir(parents=True, exist_ok=True)
        self._path(dev_id, ext).write_bytes(data)
        view = self.get(dev_id)
        view["model"] = {"name": Path(name).name[:100], "type": ext, "bytes": len(data)}
        self._write(dev_id, view)
        return view

    def model_path(self, dev_id: str) -> Path | None:
        model = self.get(dev_id).get("model")
        if not model:
            return None
        path = self._path(dev_id, model["type"])
        return path if path.exists() else None

    def delete_model(self, dev_id: str) -> dict:
        view = self.get(dev_id)
        for ext in MODEL_TYPES:
            self._path(dev_id, ext).unlink(missing_ok=True)
        if view.pop("model", None) is not None:
            self._write(dev_id, view)
        return view
