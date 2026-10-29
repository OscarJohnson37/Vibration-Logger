"""Feature definitions (protocol/features/*.json) and set_config validation.

The master checks every config change against the feature definitions, and against the limits a
logger advertises in its info, before sending set_config, so a bad value is caught on the
dashboard instead of on the device.
"""

from __future__ import annotations

import json
import os
from pathlib import Path


def default_features_dir() -> Path:
    """protocol/features in the repo, or LOGGER_FEATURES_DIR."""
    env = os.environ.get("LOGGER_FEATURES_DIR")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2] / "protocol" / "features"


class Features:
    def __init__(self, folder: str | Path | None = None):
        self.folder = Path(folder) if folder else default_features_dir()
        self.defs: dict[str, dict] = {}
        for p in sorted(self.folder.glob("*.json")):
            d = json.loads(p.read_text(encoding="utf-8"))
            self.defs[d["feature"]] = d
        if not self.defs:
            raise FileNotFoundError(f"no feature definitions in {self.folder}")

    def get(self, feature: str) -> dict | None:
        return self.defs.get(feature)

    def validate_changes(self, info: dict | None, changes: dict) -> tuple[list[str], list[str]]:
        """Check a set_config `changes` object for one logger.

        Returns (errors, restart_fields). restart_fields lists "instance.field" changes the
        definitions mark "applies": "restart".
        """
        errors: list[str] = []
        restart: list[str] = []
        if not isinstance(changes, dict) or not changes:
            return ["changes must be a non-empty object {instance: {field: value}}"], restart
        instances = {f["instance"]: f for f in (info or {}).get("features", [])}
        for inst, fields in changes.items():
            entry = instances.get(inst)
            if entry is None:
                errors.append(f"this logger has no feature instance '{inst}'")
                continue
            d = self.defs.get(entry["feature"])
            if d is None:
                errors.append(f"unknown feature '{entry['feature']}' (no definition)")
                continue
            if not isinstance(fields, dict):
                errors.append(f"{inst}: changes must be an object of fields")
                continue
            limits = entry.get("limits") or {}
            for name, value in fields.items():
                spec = d["config"].get(name)
                if spec is None:
                    errors.append(f"{inst}.{name}: not a {entry['feature']} setting")
                    continue
                errors += check_value(f"{inst}.{name}", spec, value, limits.get(name))
                if spec.get("applies") == "restart":
                    restart.append(f"{inst}.{name}")
        return errors, restart


_TYPES = {
    "string": (str,), "boolean": (bool,), "integer": (int,), "number": (int, float),
    "array": (list,), "object": (dict,),
}


def check_value(path: str, spec: dict, value, allowed: list | None = None) -> list[str]:
    """Errors for one value against a definition field (type, enum/limits, min/max, max_length)."""
    t = spec.get("type")
    if value is None:
        return [] if spec.get("default", 0) is None else [f"{path}: may not be null"]
    if t in _TYPES:
        ok = isinstance(value, _TYPES[t]) and not (t in ("integer", "number") and isinstance(value, bool))
        if not ok:
            return [f"{path}: must be {'an' if t[0] in 'aeiou' else 'a'} {t}"]
    errs = []
    choices = allowed if allowed is not None else spec.get("enum")
    if choices is not None and value not in choices:
        errs.append(f"{path}: must be one of {choices}")
    if "min" in spec and isinstance(value, (int, float)) and value < spec["min"]:
        errs.append(f"{path}: must be at least {spec['min']}")
    if "max" in spec and isinstance(value, (int, float)) and value > spec["max"]:
        errs.append(f"{path}: must be at most {spec['max']}")
    if "max_length" in spec and isinstance(value, str) and len(value) > spec["max_length"]:
        errs.append(f"{path}: at most {spec['max_length']} characters")
    item = spec.get("items")
    if t == "array" and isinstance(item, dict):
        for i, v in enumerate(value):
            errs += check_value(f"{path}[{i}]", item, v)
    sub = spec.get("fields")
    if t == "object" and sub:
        for k, v in value.items():
            if k in sub:
                errs += check_value(f"{path}.{k}", sub[k], v)
            else:
                errs.append(f"{path}.{k}: unknown field")
    return errs
