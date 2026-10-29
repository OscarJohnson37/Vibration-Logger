"""Simulated vibration loggers speaking MQTT v1, for developing the master without hardware.

Each simulated logger behaves like the real firmware as far as the master can tell: retained
info/status/config with a last-will, model VL-P1 rev A, sessions, acks for every command, an
accel_1s stats publication every second whether or not it's recording (a slowly tilting gravity
vector plus vibration), and a battery that drains. Like the real logger, it doesn't auto-start
while connected to the master (unless recording.autostart_with_master is set): the countdown is
cancelled and it waits for a start command.

OTA updates (protocol/features/ota.json) are simulated end to end: `update` really downloads the
image from the master over HTTP and checks its size and SHA-256, then "reboots" into the new
version and confirms it. A version containing "bad" never becomes healthy, so it rolls back.
"""

from __future__ import annotations

import hashlib
import json
import math
import random
import re
import threading
import time
import urllib.request
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

from .features import Features

FW = "0.3.0"                  # simulated firmware version before any OTA update
MODEL = "VL-P1"
MAX_IMAGE_BYTES = 3342336
SIM_CONFIRM_S = 8             # simulated "healthy" check after an update (the real one waits for MQTT)
SESSION_CHARS = re.compile(r"[^A-Za-z0-9_-]")


def session_slug(name: str) -> str:
    return SESSION_CHARS.sub("_", name)[:40]


class SimLogger:
    def __init__(self, index: int, broker: str, port: int, features: Features, status_interval_s: int):
        self.i = index
        self.id = f"5e00000000{index:02x}"
        self.serial = f"SIM-{index + 1:04d}"
        self.features = features
        self.boot_ms = time.time() * 1000
        self.boot_id = 1
        self.rng = random.Random(index)
        self.config = {
            "core": {"name": f"sim-{index + 1:02d}", "groups": ["sim", f"bench-{'ab'[index % 2]}"],
                     "mqtt_host": "", "status_interval_s": status_interval_s, "led_brightness": 0.2},
            "recording": {"autostart": True, "autostart_with_master": False, "start_delay_s": 10,
                          "session": "", "flush_interval_s": 5,
                          "file_max_minutes": 60, "file_max_mb": 256},
            "sd": {"bus_width": 1, "freq_khz": 20000},
            "battery": {"medium_pct": 70, "low_warning_pct": 30, "critical_pct": 10, "log_interval_s": 10},
            "wifi": {"networks": [{"ssid": "logger-net"}], "hotspot": "no_master"},
            "accel": {"range_g": 8, "odr_hz": 4000, "mounting": {"location": ""}},
            "publish": {"budget_kbps": 32, "publications": [
                {"name": "accel_1s", "stream": "accel", "method": "stats",
                 "params": {"stats": ["mean", "ac_rms", "peak"]}, "interval_s": 1}]},
            "ota": {"confirm_timeout_s": 300},
        }
        self.synced = index != 1            # one logger starts with an unset clock
        self.sync_ms = time.time() * 1000
        self.usb = index == 2               # one logger is on USB, charging
        self.percent = 95.0 - 17 * index % 80
        self.recording = False
        self.session = ""
        self.file_index = 0
        self.files_this_boot = 0
        self.started_ms: float | None = None
        self.countdown_until = time.monotonic() + self.config["recording"]["start_delay_s"]
        self.autostart = "counting"
        self.identify_until = 0.0
        self.frames = 0
        self.sent = 0
        self.tilt_phase = self.rng.uniform(0, 6.28)

        self.broker, self.port = broker, port
        self.firmware = FW
        self.ota = {"state": "idle"}

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"logger-{self.id}")
        self.client.will_set(self.topic("status"), json.dumps({"v": 1, "state": "offline"}), qos=1, retain=True)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect_async(broker, port, keepalive=30)
        self.lock = threading.Lock()

    def topic(self, t: str) -> str:
        return f"loggers/{self.id}/{t}"

    # ------------------------------------------------------------ MQTT

    def _on_connect(self, client, userdata, flags, reason, properties=None):
        if reason.is_failure:
            return
        client.subscribe(self.topic("cmd"), qos=1)
        client.subscribe("loggers/all/cmd", qos=1)
        for g in self.config["core"]["groups"]:
            client.subscribe(f"loggers/group/{g}/cmd", qos=1)
        self.publish_info()
        self.publish_config()
        self.publish_status()

    def _on_message(self, client, userdata, msg):
        try:
            cmd = json.loads(msg.payload)
        except ValueError:
            return
        with self.lock:
            ack = {"v": 1, "id": cmd.get("id")}
            try:
                ok, result, error, message = self.command(cmd.get("cmd", ""), cmd.get("target"), cmd.get("args") or {})
            except Exception as e:  # never leave the master waiting
                ok, result, error, message = False, {}, "failed", str(e)
            ack["ok"] = ok
            if ok:
                ack["result"] = result
            else:
                ack["error"] = error
                if message:
                    ack["message"] = message
        self.client.publish(self.topic("cmd/ack"), json.dumps(ack), qos=1)
        if ok and cmd.get("cmd") == "reboot":
            threading.Timer(1.0, self.reboot).start()

    # ------------------------------------------------------------ messages

    def info(self) -> dict:
        feats = [
            {"instance": "core", "feature": "core", "version": 1},
            {"instance": "recording", "feature": "recording", "version": 1},
            {"instance": "sd", "feature": "sd", "version": 1},
            {"instance": "battery", "feature": "battery", "version": 1, "impl": "MAX17048"},
            {"instance": "wifi", "feature": "wifi", "version": 1},
            {"instance": "accel", "feature": "accel", "version": 1, "impl": "sim",
             "limits": {"range_g": [2, 4, 8], "odr_hz": [125, 250, 500, 1000, 2000, 4000]}},
            {"instance": "publish", "feature": "publish", "version": 1, "methods": ["latest", "mean", "stats"]},
            {"instance": "ota", "feature": "ota", "version": 1},
        ]
        rate = self.config["accel"]["odr_hz"]
        scale = self.config["accel"]["range_g"] / 512000
        return {
            "v": 1,
            "device": {"id": self.id, "serial": self.serial, "name": self.config["core"]["name"],
                       "type": "vibration", "model": "VL-P1", "hardware_rev": "A",
                       "hardware": "simulator", "firmware": self.firmware},
            "features": feats,
            "streams": [{"id": 1, "name": "accel", "feature": "accel", "rate_hz": rate, "dtype": "i24",
                         "raw_min": -524288, "raw_max": 524287,
                         "channels": [{"name": n, "unit": "g", "scale": scale, "offset": 0} for n in "xyz"]}],
            "groups": self.config["core"]["groups"],
            "network": {"interface": "wifi", "ip": f"10.0.0.{50 + self.i}",
                        "mac": ":".join(self.id[k:k + 2] for k in range(0, 12, 2))},
        }

    def utc_ms(self) -> int | None:
        return int(time.time() * 1000) if self.synced else None

    def status(self) -> dict:
        uptime = int(time.time() * 1000 - self.boot_ms)
        counting = self.countdown_until is not None and not self.recording
        state = "recording" if self.recording else "starting" if counting else "idle"
        rec = {"recording": self.recording, "autostart": self.autostart,
               "session": self.session or self.config["recording"]["session"],
               "files_this_boot": self.files_this_boot, "bytes_written": self.frames * 9}
        if self.recording:
            rec["file"] = self.file_name()
            rec["started_ms"] = self.started_ms
        elif counting:
            rec["countdown_s"] = max(0, int(self.countdown_until - time.monotonic()))
        volts = 3.3 + 0.9 * self.percent / 100
        level = ("critical" if self.percent < 10 else "low" if self.percent < 30
                 else "medium" if self.percent < 70 else "high")
        return {
            "v": 1, "t": self.utc_ms(), "uptime_ms": uptime, "state": state, "boot_id": self.boot_id,
            "errors": [],
            "features": {
                "core": {"temperature_c": round(28 + 3 * math.sin(time.time() / 600 + self.i), 1),
                         "time": {"synced": self.synced, "source": "ntp" if self.synced else None,
                                  "last_sync_age_s": int((time.time() * 1000 - self.sync_ms) / 1000)}
                         if self.synced else {"synced": False},
                         "free_heap": 230000},
                "recording": rec,
                "sd": {"present": True, "free_mb": 29500 - self.frames // 100000, "total_mb": 29808,
                       "write_errors": 0, "bus_width": 1,
                       **({"file": f"/logs/{self.session_dir()}/{self.file_name()}"} if self.recording else {})},
                "battery": {"present": True, "voltage": round(volts, 3), "percent": round(self.percent, 1),
                            "level": level, "charge_rate_pct_per_h": 20.0 if self.usb else -6.0,
                            "charging": self.usb and self.percent < 99, "usb_power": self.usb,
                            "full": self.usb and self.percent >= 99},
                "wifi": {"mode": "station", "connected": True, "master": True, "ssid": "logger-net",
                         "ip": f"10.0.0.{50 + self.i}", "rssi_dbm": -55 - 6 * self.i + self.rng.randint(-3, 3)},
                "accel": {"ok": True, "frames": self.frames, "gaps": 0, "fifo_overflows": 0,
                          "clipped_blocks": 0, "errors": 0, "resets": 0, "temperature_c": 26.5},
                "publish": {"publications": [{"name": "accel_1s", "sent": self.sent, "skipped": 0}]},
                "ota": self.ota_status(),
            },
        }

    def ota_status(self) -> dict:
        o = {k: v for k, v in self.ota.items() if not k.startswith("_")}
        o["version"] = self.firmware
        if o["state"] == "confirming":
            o["confirm_s"] = max(0, int(self.ota["_confirm_until"] - time.monotonic()))
        return o

    def session_dir(self) -> str:
        return session_slug(self.session) if self.session else "unnamed"

    def file_name(self) -> str:
        return f"{self.serial}_b{self.boot_id:06d}_f{self.file_index:04d}.dlog"

    def publish_info(self):
        self.client.publish(self.topic("info"), json.dumps(self.info()), qos=1, retain=True)

    def publish_config(self):
        self.client.publish(self.topic("config"), json.dumps({"v": 1, **self.config}), qos=1, retain=True)

    def publish_status(self):
        self.client.publish(self.topic("status"), json.dumps(self.status()), qos=1, retain=True)

    def event(self, name: str, source: str, detail: dict | None = None):
        body = {"v": 1, "t": self.utc_ms(), "uptime_ms": int(time.time() * 1000 - self.boot_ms),
                "name": name, "source": source}
        if detail:
            body["detail"] = detail
        self.client.publish(self.topic("events"), json.dumps(body), qos=1)

    # ------------------------------------------------------------ behaviour

    def start_recording(self, reason: str):
        configured = self.config["recording"]["session"]
        if configured:
            self.session = configured
        elif self.synced:
            self.session = datetime.now(timezone.utc).strftime("%Y-%m-%d_%H%MZ")
        else:
            self.session = ""
        self.recording = True
        self.countdown_until = None
        self.file_index = self.files_this_boot
        self.files_this_boot += 1
        self.started_ms = self.utc_ms()
        self.event("recording_started", "recording", {"reason": reason, "session": self.session_dir()})

    def stop_recording(self):
        self.recording = False
        self.countdown_until = None
        self.event("recording_stopped", "recording")

    def reboot(self):
        with self.lock:
            self.boot_id += 1
            self.boot_ms = time.time() * 1000
            self.recording = False
            self.files_this_boot = 0
            self.frames = 0
            self.countdown_until = (time.monotonic() + self.config["recording"]["start_delay_s"]
                                    if self.config["recording"]["autostart"] else None)
            self.autostart = "counting" if self.countdown_until else "off"
            self.publish_info()
            self.publish_config()
            self.publish_status()

    def command(self, cmd: str, target: str | None, args: dict):
        owners = {"identify": "core", "mark": "core", "get_config": "core", "set_config": "core",
                  "sync_time": "core", "set_time": "core", "reboot": "core", "start": "recording",
                  "stop": "recording", "roll_file": "recording", "publish": "publish", "update": "ota"}
        self_tests = ["accel", "sd"]
        if cmd == "self_test":
            if target is None:
                return False, {}, "ambiguous_target", "more than one instance provides 'self_test'; set target"
            if target not in self_tests:
                return False, {}, "unknown_target", f"no feature instance '{target}'"
        elif cmd not in owners:
            return False, {}, "unsupported", f"no feature provides '{cmd}'"
        elif target and target != owners[cmd]:
            return False, {}, "unsupported", f"'{target}' has no command '{cmd}'"

        if cmd == "identify":
            self.identify_until = time.monotonic() + int(args.get("seconds", 10))
            return True, {}, "", ""
        if cmd == "mark":
            self.event("note", "core", {"text": str(args.get("text", ""))})
            return True, {}, "", ""
        if cmd == "get_config":
            self.publish_config()
            return True, {}, "", ""
        if cmd == "set_config":
            changes = args.get("changes")
            errors, restart = self.features.validate_changes(self.info(), changes)
            if errors:
                return False, {}, "invalid_args", "; ".join(errors)
            for inst, fields in changes.items():
                self.config.setdefault(inst, {}).update(fields)
            self.publish_config()
            self.event("config_changed", "core", {"changes": list(changes)})
            if "core" in changes and "groups" in changes["core"]:
                self.publish_info()
            return True, {"restart_required": bool(restart)}, "", ""
        if cmd in ("sync_time", "set_time"):
            self.synced = True
            self.sync_ms = time.time() * 1000
            self.event("time_synced", "core", {"source": "ntp" if cmd == "sync_time" else "manual"})
            return True, {}, "", ""
        if cmd == "reboot":
            return True, {}, "", ""
        if cmd == "start":
            if "session" in args:
                name = str(args["session"])
                if not 1 <= len(name) <= 40:
                    return False, {}, "invalid_args", "session must be 1-40 characters"
                self.config["recording"]["session"] = session_slug(name)
                self.publish_config()
                if self.recording:
                    self.stop_recording()
            if not self.recording:
                self.start_recording("command")
            self.autostart = "off"
            return True, {"session": self.session_dir(), "file": self.file_name()}, "", ""
        if cmd == "stop":
            if self.recording:
                self.stop_recording()
            self.autostart = "off"
            return True, {}, "", ""
        if cmd == "roll_file":
            if not self.recording:
                return False, {}, "busy", "not recording"
            self.file_index = self.files_this_boot
            self.files_this_boot += 1
            self.event("file_rolled", "recording", {"file": self.file_name()})
            return True, {"file": self.file_name()}, "", ""
        if cmd == "update":
            return self.start_update(args)
        if cmd == "self_test" and target == "accel":
            if self.recording:
                return False, {}, "busy", "stop recording first"
            return True, {"delta_g": [0.31, 0.29, 1.52], "passed": True,
                          "criteria": "provisional: every axis moves > 0.05 g"}, "", ""
        if cmd == "self_test" and target == "sd":
            return True, {"mounted": True, "card_type": "SDHC/SDXC", "bus_width": 1, "freq_khz": 20000,
                          "total_mb": 29808, "free_mb": 29500, "read_write_ok": True, "write_kBps": 950.0,
                          "read_kBps": 1480.0, "log_files": self.files_this_boot, "passed": True}, "", ""
        return False, {}, "unsupported", f"'{cmd}' not simulated"

    # ------------------------------------------------------------ OTA

    def start_update(self, args: dict):
        if self.recording:
            return False, {}, "busy", "recording: stop it first"
        if self.ota["state"] in ("downloading", "verifying", "rebooting", "confirming"):
            return False, {}, "busy", f"an update is already {self.ota['state']}"
        version, sha, size = str(args.get("version", "")), str(args.get("sha256", "")), args.get("size")
        if not version or not (args.get("path") or args.get("url")):
            return False, {}, "invalid_args", "version and path (or url) are required"
        if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            return False, {}, "invalid_args", "sha256 must be 64 lowercase hex digits"
        if not isinstance(size, int) or not 0 < size <= MAX_IMAGE_BYTES:
            return False, {}, "invalid_args", f"size must be 1-{MAX_IMAGE_BYTES} bytes"
        if args.get("model") != MODEL:
            return False, {}, "invalid_args", f"firmware is for {args.get('model')}, this logger is {MODEL}"
        # Like the firmware: the master's address is the host this logger reached the broker at
        url = args.get("url") or f"http://{self.broker}:{int(args.get('port', 8080))}{args['path']}"
        self.ota = {"state": "downloading", "progress_pct": 0, "target_version": version}
        self.event("update_started", "ota", {"version": version})
        threading.Thread(target=self._download, args=(url, version, sha, size), daemon=True).start()
        return True, {"accepted": True}, "", ""

    def _download(self, url: str, version: str, sha: str, size: int):
        h, got = hashlib.sha256(), 0
        try:
            with urllib.request.urlopen(url, timeout=10) as r:
                length = int(r.headers.get("Content-Length") or 0)
                if length != size:
                    raise ValueError(f"server sent {length} bytes, expected {size}")
                while True:
                    chunk = r.read(max(4096, size // 20))
                    if not chunk:
                        break
                    h.update(chunk)
                    got += len(chunk)
                    with self.lock:
                        self.ota["progress_pct"] = int(100 * got / size)
                        self.publish_status()
                    time.sleep(0.4)               # spread over a few seconds, like a real logger
            with self.lock:
                self.ota = {"state": "verifying", "target_version": version}
                self.publish_status()
            time.sleep(0.5)
            if got != size or h.hexdigest() != sha:
                raise ValueError("checksum mismatch")
        except Exception as e:
            with self.lock:
                self.ota = {"state": "failed", "last_error": str(e)}
                self.event("update_failed", "ota", {"version": version, "error": str(e)})
                self.publish_status()
            return
        with self.lock:
            self.ota = {"state": "rebooting", "target_version": version}
            self.publish_status()
        time.sleep(1)
        previous = self.firmware
        self.firmware = version
        self.ota = {"state": "confirming", "target_version": version, "previous_version": previous,
                    "_confirm_until": time.monotonic() + SIM_CONFIRM_S}
        self.reboot()
        time.sleep(SIM_CONFIRM_S)
        if "bad" in version:                      # never got healthy: back to the old firmware
            self.firmware = previous
            self.ota = {"state": "idle", "previous_version": version,
                        "last_error": "new firmware didn't become healthy; rolled back"}
            self.reboot()
            with self.lock:
                self.event("update_rolled_back", "ota", {"version": version, "previous_version": previous,
                                                         "reason": "not healthy within confirm_timeout_s"})
            return
        with self.lock:
            self.ota = {"state": "idle", "previous_version": previous}
            self.event("update_installed", "ota", {"version": version, "previous_version": previous})
            self.publish_status()

    def gravity(self, t: float) -> list[float]:
        """A slowly tilting board: gravity mostly on z, rocking about x and y."""
        a = 0.5 * math.sin(t / 20 + self.tilt_phase)
        b = 0.35 * math.sin(t / 31 + 2 * self.tilt_phase)
        return [math.sin(b), -math.sin(a) * math.cos(b), math.cos(a) * math.cos(b)]

    def tick(self, t: float):
        with self.lock:
            if self.countdown_until is not None and not self.recording:
                if self.client.is_connected() and not self.config["recording"]["autostart_with_master"]:
                    # Connected to the master: it decides when to record
                    self.countdown_until = None
                    self.autostart = "skipped_master"
                    self.event("autostart_skipped", "recording", {"reason": "master connected"})
                    self.publish_status()
                elif time.monotonic() >= self.countdown_until:
                    self.start_recording("autostart")
                    self.autostart = "started"
                    self.publish_status()
            drain = 20 / 3600 if self.usb else -6 / 3600
            self.percent = min(100.0, max(0.0, self.percent + drain))
            rate = self.config["accel"]["odr_hz"]
            if self.recording:
                self.frames += rate
            g = self.gravity(t)
            level = 0.02 + 0.015 * self.i + 0.01 * math.sin(t / 7)
            rms = [round(level * k * (1 + 0.1 * self.rng.random()), 5) for k in (0.6, 0.8, 1.0)]
            msg = {
                "v": 1, "t": self.utc_ms(), "uptime_ms": int(time.time() * 1000 - self.boot_ms),
                "name": "accel_1s", "stream": "accel", "method": "stats",
                "params": {"stats": ["mean", "ac_rms", "peak"]},
                "window": {"t_mono_us": int((time.time() * 1000 - self.boot_ms) * 1000) - 1_000_000,
                           "duration_s": 1.0, "frames": rate, "flags": []},
                "channels": ["x", "y", "z"], "unit": "g", "axes": "sensor",
                "values": {"mean": [round(v, 5) for v in g], "ac_rms": rms,
                           "peak": [round(r * (2.8 + 0.4 * self.rng.random()), 5) for r in rms]},
            }
        self.client.publish(self.topic("data/accel_1s"), json.dumps(msg), qos=0)
        self.sent += 1


def run(broker: str, port: int = 1883, count: int = 3, status_interval_s: int = 10) -> None:
    features = Features()
    sims = [SimLogger(i, broker, port, features, status_interval_s) for i in range(count)]
    for s in sims:
        s.client.loop_start()
    print(f"{count} simulated loggers on {broker}:{port}: " + ", ".join(s.serial for s in sims))
    t0 = time.monotonic()
    last_status = 0.0
    try:
        while True:
            t = time.monotonic() - t0
            for s in sims:
                s.tick(t)
            if t - last_status >= status_interval_s:
                last_status = t
                for s in sims:
                    with s.lock:
                        s.publish_status()
            time.sleep(max(0.0, 1 - (time.monotonic() - t0 - t)))
    except KeyboardInterrupt:
        pass
    finally:
        for s in sims:  # a clean exit looks like a power cut to the master: mark them offline
            s.client.publish(s.topic("status"), json.dumps({"v": 1, "state": "offline"}), qos=1, retain=True)
            s.client.loop_stop()
            s.client.disconnect()
