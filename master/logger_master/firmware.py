"""Firmware images for over-the-air updates (protocol/features/ota.json), kept on the master:

    registry/firmware/<model>/<version>.bin
    registry/firmware/index.json      { "<model>/<version>": { model, version, size, sha256, ... } }

Loggers download images from GET /firmware/<model>/<version>.bin when the master sends them an
`update` command. Each image is checked here before it's stored: an ESP32 app image (magic 0xE9),
small enough for an OTA slot, and, if it carries the firmware's marker
"VLFW-MODEL=<model>;VLFW-VERSION=<version>;", made for the model it's filed under.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import time
from pathlib import Path

MAX_IMAGE_BYTES = 3342336            # one OTA app slot (0x330000) on the 8 MB TinyS3[D]
ESP_IMAGE_MAGIC = 0xE9
_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,39}$")
_MARKER = re.compile(rb"VLFW-MODEL=([A-Za-z0-9._-]{1,40});VLFW-VERSION=([A-Za-z0-9._-]{1,40});")


def read_marker(data: bytes) -> tuple[str, str] | None:
    """(model, version) from the firmware's embedded marker, or None if it has none."""
    m = _MARKER.search(data)
    return (m.group(1).decode(), m.group(2).decode()) if m else None


class FirmwareStore:
    def __init__(self, folder: Path):
        self.folder = Path(folder)

    def _index_path(self) -> Path:
        return self.folder / "index.json"

    def _index(self) -> dict:
        try:
            return json.loads(self._index_path().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _write_index(self, index: dict) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        tmp = self._index_path().with_suffix(".tmp")
        tmp.write_text(json.dumps(index, indent=1), encoding="utf-8")
        os.replace(tmp, self._index_path())

    @staticmethod
    def _check_names(model: str, version: str) -> None:
        if not _NAME.match(model or ""):
            raise ValueError("model must be 1-40 characters: letters, digits, . _ -")
        if not _NAME.match(version or ""):
            raise ValueError("version must be 1-40 characters: letters, digits, . _ -")

    def path(self, model: str, version: str) -> Path | None:
        self._check_names(model, version)
        p = self.folder / model / f"{version}.bin"
        return p if p.exists() and f"{model}/{version}" in self._index() else None

    def list(self) -> list[dict]:
        return sorted(self._index().values(), key=lambda e: (e["model"], e.get("uploaded", 0)), reverse=False)

    def get(self, model: str, version: str) -> dict | None:
        self._check_names(model, version)
        return self._index().get(f"{model}/{version}")

    def save(self, model: str, version: str, data: bytes, filename: str = "", notes: str = "") -> dict:
        """Validate and store an image; replaces an existing one with the same model and version."""
        self._check_names(model, version)
        if filename and not filename.lower().endswith(".bin"):
            raise ValueError("firmware must be a .bin file (PlatformIO: .pio/build/<env>/firmware.bin)")
        if not data:
            raise ValueError("empty file")
        if len(data) > MAX_IMAGE_BYTES:
            raise ValueError(f"image is {len(data)} bytes; the OTA slot holds {MAX_IMAGE_BYTES}")
        if data[0] != ESP_IMAGE_MAGIC:
            raise ValueError("not an ESP32 firmware image (use firmware.bin, not a merged or bootloader image)")
        marker = read_marker(data)
        if marker and marker[0] != model:
            raise ValueError(f"this image is for model {marker[0]}, not {model}")
        if marker and marker[1] != version:
            raise ValueError(f"this image says it's version {marker[1]}, not {version}")
        (self.folder / model).mkdir(parents=True, exist_ok=True)
        dest = self.folder / model / f"{version}.bin"
        tmp = dest.with_suffix(".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, dest)
        entry = {
            "model": model, "version": version, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(), "uploaded": time.time(),
            "filename": Path(filename).name[:100] if filename else "", "notes": (notes or "")[:500],
            "marker": bool(marker),
        }
        index = self._index()
        index[f"{model}/{version}"] = entry
        self._write_index(index)
        return entry

    def delete(self, model: str, version: str) -> bool:
        self._check_names(model, version)
        index = self._index()
        found = index.pop(f"{model}/{version}", None) is not None
        (self.folder / model / f"{version}.bin").unlink(missing_ok=True)
        if found:
            self._write_index(index)
        return found
