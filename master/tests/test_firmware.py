"""Tests for OTA firmware updates on the master: the image store, the API and the fleet fan-out
(protocol/features/ota.json)."""

import asyncio
import hashlib
import json
import threading
import time

import pytest

from logger_master.features import Features
from logger_master.firmware import MAX_IMAGE_BYTES, FirmwareStore, read_marker
from logger_master.fleet import Fleet


def image(model="VL-P1", version="0.4.0", size=4096, marker=True) -> bytes:
    """Something shaped like an ESP32 app image: magic byte first, the firmware's marker inside."""
    body = bytearray(size)
    body[0] = 0xE9
    if marker:
        m = f"VLFW-MODEL={model};VLFW-VERSION={version};".encode()
        body[100:100 + len(m)] = m
    return bytes(body)


# ------------------------------------------------------------ store

def test_store_round_trip(tmp_path):
    store = FirmwareStore(tmp_path)
    data = image()
    e = store.save("VL-P1", "0.4.0", data, "firmware.bin", "first OTA build")
    assert e["sha256"] == hashlib.sha256(data).hexdigest() and e["size"] == len(data) and e["marker"]
    assert store.path("VL-P1", "0.4.0").read_bytes() == data
    assert [i["version"] for i in FirmwareStore(tmp_path).list()] == ["0.4.0"]   # survives a reload
    store.save("VL-P1", "0.4.0", image(size=5000))                               # same version: replaced
    assert store.get("VL-P1", "0.4.0")["size"] == 5000
    assert store.delete("VL-P1", "0.4.0") and store.path("VL-P1", "0.4.0") is None
    assert not store.delete("VL-P1", "0.4.0")


@pytest.mark.parametrize("model,version,data,filename,why", [
    ("VL-P1", "0.4.0", b"", "", "empty"),
    ("VL-P1", "0.4.0", b"\x00" * 100, "", "not an ESP32 firmware image"),
    ("VL-P1", "0.4.0", image(size=MAX_IMAGE_BYTES + 1), "", "OTA slot"),
    ("VL-P1", "0.4.0", image(model="VL-F1"), "", "for model VL-F1"),
    ("VL-P1", "0.4.1", image(version="0.4.0"), "", "says it's version 0.4.0"),
    ("VL-P1", "0.4.0", image(), "firmware.elf", ".bin"),
    ("../etc", "0.4.0", image(), "", "model must be"),
    ("VL-P1", "0.4/0", image(), "", "version must be"),
], ids=["empty", "not-esp32", "too-big", "other-model", "other-version", "not-bin", "bad-model", "bad-version"])
def test_store_rejects(tmp_path, model, version, data, filename, why):
    with pytest.raises(ValueError, match=why):
        FirmwareStore(tmp_path).save(model, version, data, filename)


def test_marker():
    assert read_marker(image("VL-P1", "0.4.0")) == ("VL-P1", "0.4.0")
    assert read_marker(image(marker=False)) is None


# ------------------------------------------------------------ fleet fan-out

def logger_info(dev_id, serial, firmware="0.3.0", model="VL-P1", ota=True):
    feats = [{"instance": "core", "feature": "core", "version": 1}]
    if ota:
        feats.append({"instance": "ota", "feature": "ota", "version": 1})
    return {"v": 1, "device": {"id": dev_id, "serial": serial, "type": "vibration", "model": model,
                               "firmware": firmware}, "features": feats, "groups": []}


class FakeClient:
    def __init__(self):
        self.sent = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.sent.append((topic, json.loads(payload)))


def test_update_fan_out_and_summary(tmp_path):
    async def run():
        fleet = Fleet("localhost", features=Features())
        fleet._client = FakeClient()
        for dev, serial, kw in [("a1", "VL-0001", {}), ("a2", "VL-0002", {}), ("a3", "VL-0003", {"firmware": "0.4.0"}),
                                ("a4", "VL-0004", {"model": "VL-F1"}), ("a5", "VL-0005", {"ota": False})]:
            fleet._handle(f"loggers/{dev}/info", json.dumps(logger_info(dev, serial, **kw)).encode(), False)
        acks = {"a1": {"ok": True, "result": {"accepted": True}},
                "a2": {"ok": False, "error": "busy", "message": "recording: stop it first"}}
        orig = fleet._client.publish

        def publish(topic, payload, qos=0, retain=False):
            orig(topic, payload, qos, retain)
            msg, dev = json.loads(payload), topic.split("/")[1]
            asyncio.get_running_loop().call_soon(
                fleet._handle, f"loggers/{dev}/cmd/ack", json.dumps({"v": 1, "id": msg["id"], **acks[dev]}).encode(), False)
        fleet._client.publish = publish

        img = {"model": "VL-P1", "version": "0.4.0", "size": 4096, "sha256": "ab" * 32}
        res = await fleet.update({"all": True}, img, http_port=8080, timeout=2)
        sent_to = sorted(t.split("/")[1] for t, _ in fleet._client.sent)
        assert sent_to == ["a1", "a2"]                       # only right model, OTA-capable, not already on it
        args = fleet._client.sent[0][1]["args"]
        assert fleet._client.sent[0][1]["cmd"] == "update"
        assert args == {"path": "/firmware/VL-P1/0.4.0.bin", "port": 8080, "sha256": "ab" * 32,
                        "size": 4096, "version": "0.4.0", "model": "VL-P1"}
        s = res["summary"]
        assert s.startswith("update to 0.4.0: 1 of 2 accepted")
        assert "VL-0002 busy: recording: stop it first" in s
        assert "VL-0003 already on 0.4.0" in s
        assert "VL-0004 model VL-F1, not VL-P1" in s
        assert "VL-0005 firmware 0.3.0 can't update over WiFi" in s
        assert not res["ok"]

        none = await fleet.update({"ids": ["a3"]}, img, http_port=8080, timeout=0.2)
        assert none["summary"] == "update to 0.4.0: 0 of 0 accepted; VL-0003 already on 0.4.0"
    asyncio.run(run())


# ------------------------------------------------------------ API

def test_firmware_api(tmp_path):
    pytest.importorskip("fastapi")
    from fastapi.testclient import TestClient

    from logger_master.web import create_app

    data = image(size=70_000)
    app = create_app(Fleet("localhost", port=1), firmware=FirmwareStore(tmp_path / "firmware"), http_port=8080)
    with TestClient(app) as c:
        r = c.put("/api/firmware?model=VL-P1&version=0.4.0&filename=firmware.bin&notes=test", content=data)
        assert r.status_code == 200, r.text
        assert r.json()["sha256"] == hashlib.sha256(data).hexdigest()
        assert c.put("/api/firmware?model=VL-P1&version=0.4.0", content=b"nope").status_code == 400
        assert [i["version"] for i in c.get("/api/firmware").json()] == ["0.4.0"]

        # What a logger downloads: exact bytes, a Content-Length, no compression or chunking
        r = c.get("/firmware/VL-P1/0.4.0.bin", headers={"Accept-Encoding": "gzip"})
        assert r.status_code == 200 and r.content == data
        assert r.headers["content-length"] == str(len(data))
        assert "content-encoding" not in r.headers and r.headers.get("transfer-encoding") != "chunked"
        assert c.get("/firmware/VL-P1/9.9.9.bin").status_code == 404

        assert c.post("/api/update", json={"selection": {"all": True}, "model": "VL-P1", "version": "9.9.9"}).status_code == 404
        r = c.post("/api/update", json={"selection": {"all": True}, "model": "VL-P1", "version": "0.4.0", "timeout": 0.1})
        assert r.status_code == 200 and r.json()["summary"] == "update to 0.4.0: 0 of 0 accepted"

        assert c.delete("/api/firmware/VL-P1/0.4.0").status_code == 200
        assert c.get("/firmware/VL-P1/0.4.0.bin").status_code == 404


# ------------------------------------------------------------ simulator end to end

def test_simulated_update(tmp_path):
    """A simulated logger downloads the image over real HTTP, checks it, reboots into it and confirms."""
    pytest.importorskip("fastapi")
    import http.server

    from logger_master import simulate

    data = image(size=20_000)
    folder = tmp_path / "VL-P1"
    folder.mkdir()
    (folder / "0.4.0.bin").write_bytes(data)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(tmp_path), **kw)

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        sim = simulate.SimLogger(0, "127.0.0.1", 1, Features(), 10)   # MQTT never connects: fine here
        sim.client.publish = lambda *a, **kw: None
        simulate.SIM_CONFIRM_S = 0.2
        args = {"path": "/VL-P1/0.4.0.bin", "port": srv.server_address[1], "version": "0.4.0", "model": "VL-P1",
                "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        assert sim.command("update", None, {**args, "model": "VL-F1"})[2] == "invalid_args"
        ok, result, _, _ = sim.command("update", None, args)
        assert ok and result == {"accepted": True}
        assert sim.command("update", None, args)[2] == "busy"     # one at a time
        deadline = time.time() + 30
        while time.time() < deadline and (sim.firmware != "0.4.0" or sim.ota["state"] != "idle"):
            time.sleep(0.1)
        assert sim.firmware == "0.4.0" and sim.ota == {"state": "idle", "previous_version": "0.3.0"}

        ok, *_ = sim.command("update", None, {**args, "sha256": "0" * 64, "version": "0.4.1"})
        while time.time() < deadline and sim.ota["state"] not in ("failed", "idle"):
            time.sleep(0.1)
        assert sim.ota["state"] == "failed" and "checksum" in sim.ota["last_error"] and sim.firmware == "0.4.0"
    finally:
        srv.shutdown()
