"""The model catalogue (registry/models/<model>.md): one page per logger model with its specs,
LED guide, instructions and expected features (registry/models/README.md).

Each page is YAML front matter between --- lines, then Markdown. Pages are re-read when the
file changes, so editing one needs no restart.
"""

from __future__ import annotations

import re
from pathlib import Path

import yaml

_FRONT = re.compile(r"\A---\s*\n(.*?)\n---\s*(?:\n|\Z)", re.S)
_NAME = re.compile(r"^[A-Za-z0-9_-]{1,40}$")


def parse_page(text: str) -> tuple[dict, str]:
    """(front matter, markdown body). Pages without front matter have an empty dict."""
    m = _FRONT.match(text)
    if not m:
        return {}, text
    meta = yaml.safe_load(m.group(1)) or {}
    if not isinstance(meta, dict):
        raise ValueError("front matter must be a mapping")
    return meta, text[m.end():]


class Models:
    def __init__(self, folder: str | Path):
        self.folder = Path(folder)
        self._cache: dict[str, tuple[float, dict]] = {}

    def _load(self, path: Path) -> dict | None:
        try:
            mtime = path.stat().st_mtime
        except OSError:
            return None
        hit = self._cache.get(path.stem)
        if hit and hit[0] == mtime:
            return hit[1]
        try:
            meta, body = parse_page(path.read_text(encoding="utf-8"))
            error = None
        except (OSError, ValueError, yaml.YAMLError) as e:
            meta, body, error = {}, "", f"couldn't read {path.name}: {e}"
        model = {
            "model": str(meta.get("model") or path.stem),
            "name": meta.get("name", ""),
            "type": meta.get("type", ""),
            "hardware_revs": [str(r) for r in meta.get("hardware_revs") or []],
            "description": meta.get("description", ""),
            "expected_features": [str(f) for f in meta.get("expected_features") or []],
            "specs": meta.get("specs") or {},
            "markdown": body,
            "file": path.name,
        }
        if error:
            model["error"] = error
        self._cache[path.stem] = (mtime, model)
        return model

    def get(self, model: str | None) -> dict | None:
        if not model or not _NAME.match(model):
            return None
        return self._load(self.folder / f"{model}.md")

    def list(self) -> list[dict]:
        """Every model page, without the body."""
        out = []
        for p in sorted(self.folder.glob("*.md")):
            if p.name.lower() == "readme.md":
                continue
            m = self._load(p)
            if m:
                out.append({k: v for k, v in m.items() if k != "markdown"})
        return out

    def expected_features(self, model: str | None) -> list[str] | None:
        m = self.get(model)
        return m["expected_features"] if m and m["expected_features"] else None
