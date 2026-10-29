"""Tests for the fleet service: config validation, command acks, registry (protocol/MQTT.md)."""

import asyncio
import json

import pytest

from logger_master.features import Features
from logger_master.fleet import Fleet, LoggerState, aggregate
from logger_master.registry import Registry

INFO = {
    "v": 1,
    "device": {"id": "9070692f3338", "serial": "VL-0001", "type": "vibration", "firmware": "0.4.0"},
    "features": [
        {"instance": "core", "feature": "core", "version": 1},
        {"instance": "recording", "feature": "recording", "version": 1},
        {"instance": "wifi", "feature": "wifi", "version": 1},
        {"instance": "accel", "feature": "accel", "version": 1, "limits": {"range_g": [2, 4, 8]}},
    ],
    "groups": ["site-a"],
}


@pytest.fixture(scope="module")
def features():
    return Features()


# ------------------------------------------------------------ validation

def test_valid_changes(features):
    errors, restart = features.validate_changes(INFO, {"recording": {"start_delay_s": 30, "session": "pump2"},
                                                       "core": {"name": "vib-01", "groups": ["a", "b"]}})
    assert errors == [] and restart == []


def test_invalid_changes(features):
    errors, _ = features.validate_changes(INFO, {
        "recording": {"start_delay_s": -1, "autostart": "yes", "session": "x" * 41, "nope": 1},
        "accel": {"range_g": 16},
        "env": {"interval_s": 1},
    })
    text = "\n".join(errors)
    assert "start_delay_s: must be at least 0" in text
    assert "autostart: must be a boolean" in text
    assert "session: at most 40 characters" in text
    assert "nope: not a recording setting" in text
    assert "range_g: must be one of [2, 4, 8]" in text   # the logger's limits, not the definition's enum
    assert "no feature instance 'env'" in text


def test_nested_and_types(features):
    errors, _ = features.validate_changes(INFO, {"wifi": {"networks": [{"ssid": "net", "password": "pw", "x": 1}]}})
    assert errors == ["wifi.networks[0].x: unknown field"]
    errors, _ = features.validate_changes(INFO, {"recording": {"start_delay_s": True}})
    assert errors == ["recording.start_delay_s: must be an integer"]
    errors, _ = features.validate_changes(INFO, {"recording": {"sinks": None}})
    assert errors == []   # null is the documented default (every sink)


def test_restart_fields(features):
    errors, restart = features.validate_changes(INFO, {"wifi": {"antenna": "external"}})
    assert errors == [] and restart == ["wifi.antenna"]


# ------------------------------------------------------------ acks

def lg(dev_id, serial, online=True):
    s = LoggerState(dev_id, info={"device": {"serial": serial}})
    s.online = online
    return s


def test_aggregate_summary():
    chosen = [lg("a", "VL-1"), lg("b", "VL-2"), lg("c", "VL-3", online=False), lg("d", "VL-4"), lg("e", "VL-5")]
    acks = {
        "a": {"ok": True, "result": {"session": "s1"}},
        "b": {"ok": False, "error": "busy", "message": "stop recording first"},
        "e": {"ok": False, "error": "unsupported", "message": "no feature provides 'start'"},
    }
    labels = {x.device_id: x.label for x in chosen}
    r = aggregate("start", chosen, acks, labels)
    status = {x["device_id"]: x["status"] for x in r["results"]}
    assert status == {"a": "ok", "b": "error", "c": "offline", "d": "no_reply", "e": "not_applicable"}
    assert r["summary"] == ("start: 1 of 4 ok; VL-2 busy: stop recording first; VL-3 offline; "
                            "VL-4 didn't reply; 1 without this command")
    assert not r["ok"]


class FakeClient:
    def __init__(self):
        self.sent = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.sent.append((topic, json.loads(payload)))


def make_fleet(tmp_path, features):
    f = Fleet("localhost", registry=Registry(tmp_path / "devices.yaml"), features=features)
    f._client = FakeClient()
    return f


def test_fleet_tracks_and_commands(tmp_path, features):
    async def run():
        fleet = make_fleet(tmp_path, features)
        events = []
        fleet.listen(events.append)
        fleet._handle("loggers/9070692f3338/info", json.dumps(INFO).encode(), True)
        fleet._handle("loggers/9070692f3338/status", json.dumps({"v": 1, "state": "recording"}).encode(), True)
        lg_ = fleet.loggers["9070692f3338"]
        assert lg_.online is None                    # retained only: not confirmed yet
        fleet._handle("loggers/9070692f3338/status", json.dumps({"v": 1, "state": "idle"}).encode(), False)
        assert lg_.online is True and lg_.status["state"] == "idle"

        # the logger acks as soon as the command is published
        orig = fleet._client.publish
        def publish(topic, payload, qos=0, retain=False):
            orig(topic, payload, qos, retain)
            cid = json.loads(payload)["id"]
            asyncio.get_running_loop().call_soon(fleet._handle, "loggers/9070692f3338/cmd/ack",
                                                 json.dumps({"v": 1, "id": cid, "ok": True, "result": {}}).encode(), False)
        fleet._client.publish = publish
        res = await fleet.command({"group": "site-a"}, "start", args={"session": "s1"}, timeout=2)
        assert fleet._client.sent[-1][0] == "loggers/group/site-a/cmd"
        assert fleet._client.sent[-1][1]["args"] == {"session": "s1"}
        assert res["summary"] == "start: 1 of 1 ok"

        res = await fleet.command({"all": True, "feature": "accel"}, "self_test", target="accel", timeout=2)
        assert fleet._client.sent[-1][0] == "loggers/9070692f3338/cmd"   # narrowed: sent per logger
        assert res["ok"]

        bad = await fleet.set_config("9070692f3338", {"accel": {"range_g": 16}})
        assert not bad["ok"] and "not sent" in bad["summary"]

        fleet._handle("loggers/9070692f3338/status", json.dumps({"v": 1, "state": "offline"}).encode(), True)
        assert lg_.online is False
        res = await fleet.command({"ids": ["9070692f3338"]}, "stop", timeout=0.2)
        assert res["summary"] == "stop: 0 of 1 ok; VL-0001 offline"
        assert any(e["type"] == "logger" for e in events)
    asyncio.run(run())


# ------------------------------------------------------------ registry

def test_registry_auto_add_and_mismatch(tmp_path):
    path = tmp_path / "devices.yaml"
    reg = Registry(path)
    reg.seen("9070692f3338", info=INFO)
    e = reg.get("9070692f3338")
    assert e["serial"] == "VL-0001" and e["type"] == "vibration"
    assert e["expected_features"] == ["core", "recording", "wifi", "accel"]
    assert reg.mismatches("9070692f3338", INFO) == []

    reg.update("9070692f3338", {"expected_features": ["core", "recording", "wifi", "accel", "env"],
                                "assignment": {"location": "Pump house 2", "since": "2026-10-29"}})
    less = {**INFO, "features": INFO["features"][:3]}
    assert reg.mismatches("9070692f3338", less) == [
        "expected 'accel' but the logger doesn't report it",
        "expected 'env' but the logger doesn't report it",
    ]
    with pytest.raises(ValueError):
        reg.update("9070692f3338", {"last_seen": {}})

    again = Registry(path)          # survives a reload, comments and all
    assert again.get("9070692f3338")["assignment"]["location"] == "Pump house 2"
    assert path.read_text(encoding="utf-8").startswith("# Logger registry")
