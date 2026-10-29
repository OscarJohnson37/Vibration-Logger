"""The device registry: what the master knows about each logger that the logger can't know
about itself (docs/PLAN.md, Device identity and registry).

Stored as YAML (registry/devices.yaml) so it's easy to edit by hand and keeps version history.
New loggers are added automatically on first contact. A unit's expected features come from its
model's catalogue page (registry/models/<model>.md); `expected_features` in the unit's own entry
overrides that, for a unit built differently. A logger of a model with no catalogue page gets the
features it advertised on first contact as its expected set. Either way a feature that later
goes missing is flagged.
"""

from __future__ import annotations

import os
import time
from datetime import datetime, timezone
from pathlib import Path

import yaml

from .models import Models

HEADER = """\
# Logger registry (docs/PLAN.md, Device identity and registry).
# Loggers are added automatically on first contact; edit the manual fields freely.
#   model:             model number (registry/models/<model>.md); filled in from the logger
#   expected_features: only for a unit that differs from its model (empty: use the model's list)
#   sensors:           e.g. accel: {board_serial: ..., calibration: ...}
#   assignment:        where it's installed, and since when
# last_seen is maintained by the master.
"""

EDITABLE = ("serial", "type", "model", "hardware_rev", "expected_features", "sensors", "assignment", "notes")
SAVE_INTERVAL_S = 60


def _now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def new_entry() -> dict:
    return {
        "serial": "", "type": "", "model": "", "hardware_rev": "", "expected_features": [],
        "sensors": {}, "assignment": {"location": "", "since": ""}, "notes": "",
        "added": _now_iso(), "last_seen": {},
    }


class Registry:
    def __init__(self, path: str | Path, models: Models | None = None):
        self.path = Path(path)
        self.models = models
        self.devices: dict[str, dict] = {}
        self._dirty = False
        self._saved_at = 0.0
        if self.path.exists():
            data = yaml.safe_load(self.path.read_text(encoding="utf-8")) or {}
            for dev_id, entry in (data.get("devices") or {}).items():
                self.devices[str(dev_id)] = {**new_entry(), **(entry or {})}

    def get(self, device_id: str) -> dict | None:
        return self.devices.get(device_id)

    def seen(self, device_id: str, info: dict | None = None, status: dict | None = None) -> None:
        """Record contact with a logger; adds it on first contact."""
        entry = self.devices.get(device_id)
        if entry is None:
            entry = self.devices[device_id] = new_entry()
            self._dirty = True
        last = entry.setdefault("last_seen", {})
        if info:
            dev = info.get("device", {})
            if not entry["serial"] and dev.get("serial"):
                entry["serial"] = dev["serial"]
            for k in ("type", "model", "hardware_rev"):
                if not entry.get(k) and dev.get(k):
                    entry[k] = dev[k]
                    self._dirty = True
            # Known model: its catalogue page says what to expect. Otherwise remember what it has.
            if not entry["expected_features"] and not self.model_features(entry["model"]):
                entry["expected_features"] = [f["instance"] for f in info.get("features", [])]
                self._dirty = True
            for k, v in (("firmware", dev.get("firmware")), ("ip", (info.get("network") or {}).get("ip"))):
                if v and last.get(k) != v:
                    last[k] = v
                    self._dirty = True
        if status:
            batt = (status.get("features") or {}).get("battery") or {}
            if "percent" in batt:
                last["battery_pct"] = batt["percent"]
            last["state"] = status.get("state")
        last["time"] = _now_iso()
        self.save()

    def model_features(self, model: str | None) -> list[str] | None:
        return self.models.expected_features(model) if self.models else None

    def expected(self, device_id: str, info: dict | None = None) -> tuple[list[str], str]:
        """(expected feature instances, where the list came from: "unit", "model" or "")."""
        entry = self.devices.get(device_id) or {}
        if entry.get("expected_features"):
            return list(entry["expected_features"]), "unit"
        model = ((info or {}).get("device") or {}).get("model") or entry.get("model")
        from_model = self.model_features(model)
        return (list(from_model), "model") if from_model else ([], "")

    def mismatches(self, device_id: str, info: dict | None) -> list[str]:
        """Expected vs advertised features, as readable warnings."""
        entry = self.devices.get(device_id)
        if not entry or not info:
            return []
        have = {f["instance"] for f in info.get("features", [])}
        expected, source = self.expected(device_id, info)
        want = set(expected)
        if not source:
            return []
        out = [f"expected '{x}' but the logger doesn't report it" for x in sorted(want - have)]
        out += [f"reports '{x}', which isn't in its expected features" for x in sorted(have - want)]
        dev = info.get("device") or {}
        for k, name in (("serial", "serial"), ("model", "model"), ("hardware_rev", "hardware revision")):
            if entry.get(k) and dev.get(k) and dev[k] != entry[k]:
                out.append(f"{name} is {dev[k]} but the registry says {entry[k]}")
        return out

    def update(self, device_id: str, changes: dict) -> dict:
        """Edit the manual fields. Unknown fields are rejected."""
        bad = [k for k in changes if k not in EDITABLE]
        if bad:
            raise ValueError(f"not editable: {', '.join(bad)}")
        entry = self.devices.setdefault(device_id, new_entry())
        entry.update(changes)
        self._dirty = True
        self.save(force=True)
        return entry

    def save(self, force: bool = False) -> None:
        """Write if anything changed; last_seen-only updates at most once a minute."""
        now = time.monotonic()
        if not force and not self._dirty and now - self._saved_at < SAVE_INTERVAL_S:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        text = HEADER + yaml.safe_dump({"devices": self.devices}, sort_keys=False, allow_unicode=True)
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(text, encoding="utf-8")
        os.replace(tmp, self.path)
        self._dirty = False
        self._saved_at = now
