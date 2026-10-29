import os
import time
from pathlib import Path

import pytest

from logger_master.models import Models, parse_page
from logger_master.registry import Registry

CATALOGUE = Path(__file__).resolve().parents[2] / "registry" / "models"

PAGE = """---
model: VL-T9
name: Test logger
type: vibration
hardware_revs: [A, B]
expected_features: [core, recording, accel]
specs: {sensor: test}
---

# VL-T9

| a | b |
|---|---|
"""


def info(model="VL-T9", features=("core", "recording", "accel"), rev="A"):
    dev = {"id": "9070692f3338", "serial": "VL-0001", "type": "vibration", "firmware": "x"}
    if model:
        dev["model"] = model
        dev["hardware_rev"] = rev
    return {"v": 1, "device": dev, "features": [{"instance": f, "feature": f, "version": 1} for f in features]}


def test_parse_page():
    meta, body = parse_page(PAGE)
    assert meta["model"] == "VL-T9" and meta["hardware_revs"] == ["A", "B"]
    assert body.lstrip().startswith("# VL-T9")
    assert parse_page("# no front matter\n") == ({}, "# no front matter\n")


def test_models_load_list_and_reload(tmp_path):
    (tmp_path / "VL-T9.md").write_text(PAGE, encoding="utf-8")
    (tmp_path / "README.md").write_text("# catalogue\n", encoding="utf-8")
    models = Models(tmp_path)
    m = models.get("VL-T9")
    assert m["name"] == "Test logger" and m["expected_features"] == ["core", "recording", "accel"]
    assert "| a | b |" in m["markdown"]
    assert [x["model"] for x in models.list()] == ["VL-T9"]       # README isn't a model
    assert "markdown" not in models.list()[0]
    assert models.get("VL-NONE") is None and models.get("../secret") is None
    (tmp_path / "VL-T9.md").write_text(PAGE.replace("Test logger", "Edited"), encoding="utf-8")
    os.utime(tmp_path / "VL-T9.md", (time.time() + 5, time.time() + 5))
    assert models.get("VL-T9")["name"] == "Edited"                  # picked up without a restart


def test_bad_front_matter_reported(tmp_path):
    (tmp_path / "VL-X.md").write_text("---\n[not, a, mapping]\n---\nbody\n", encoding="utf-8")
    assert "error" in Models(tmp_path).get("VL-X")


def test_shipped_catalogue_page_is_valid():
    m = Models(CATALOGUE).get("VL-P1")
    assert m and "error" not in m
    assert set(m["expected_features"]) == {"core", "recording", "sd", "battery", "accel", "wifi", "publish", "ota"}
    assert "LED guide" in m["markdown"] and "Pin map" in m["markdown"]


def test_mismatch_uses_model_expected_features(tmp_path):
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "VL-T9.md").write_text(PAGE, encoding="utf-8")
    reg = Registry(tmp_path / "devices.yaml", models=Models(tmp_path / "models"))
    # First contact without "accel": a known model, so it isn't taken as the expected set
    reg.seen("9070692f3338", info=info(features=("core", "recording")))
    entry = reg.get("9070692f3338")
    assert entry["model"] == "VL-T9" and entry["hardware_rev"] == "A" and entry["expected_features"] == []
    assert reg.mismatches("9070692f3338", info(features=("core", "recording"))) == [
        "expected 'accel' but the logger doesn't report it"]
    assert reg.expected("9070692f3338", info())[1] == "model"
    # A unit built differently: its own list wins
    reg.update("9070692f3338", {"expected_features": ["core", "recording"]})
    assert reg.mismatches("9070692f3338", info(features=("core", "recording"))) == []
    # Reporting a different model / revision than the registry is flagged
    assert "model is VL-Z1 but the registry says VL-T9" in reg.mismatches("9070692f3338", info(model="VL-Z1"))


def test_unknown_model_falls_back_to_first_contact(tmp_path):
    reg = Registry(tmp_path / "devices.yaml", models=Models(tmp_path / "none"))
    reg.seen("9070692f3338", info=info(model=None))
    assert reg.get("9070692f3338")["expected_features"] == ["core", "recording", "accel"]
    assert reg.mismatches("9070692f3338", info(model=None, features=("core",))) == [
        "expected 'accel' but the logger doesn't report it", "expected 'recording' but the logger doesn't report it"]


def test_models_api(tmp_path):
    fastapi = pytest.importorskip("fastapi")
    from fastapi.testclient import TestClient

    from logger_master.fleet import Fleet
    from logger_master.web import create_app

    (tmp_path / "VL-T9.md").write_text(PAGE, encoding="utf-8")
    app = create_app(Fleet("localhost"), models=Models(tmp_path))
    with TestClient(app) as c:
        assert [m["model"] for m in c.get("/api/models").json()] == ["VL-T9"]
        page = c.get("/api/models/VL-T9").json()
        assert page["name"] == "Test logger" and "# VL-T9" in page["markdown"]
        assert c.get("/api/models/VL-NONE").status_code == 404
