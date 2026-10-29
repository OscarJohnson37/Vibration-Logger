"""The fleet service: tracks every logger over MQTT and sends commands (protocol/MQTT.md).

All state lives on one asyncio loop. paho-mqtt runs its network thread and hands each message
to the loop, so nothing here needs locks.
"""

from __future__ import annotations

import asyncio
import json
import logging
import secrets
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable

import paho.mqtt.client as mqtt

from .features import Features
from .registry import Registry

log = logging.getLogger(__name__)

STALE_S = 95            # three missed 30 s status reports
EVENTS_KEPT = 200
PUBLICATION_SECONDS = 600


@dataclass
class LoggerState:
    device_id: str
    info: dict = field(default_factory=dict)
    status: dict = field(default_factory=dict)
    config: dict = field(default_factory=dict)
    online: bool | None = None          # None: only retained messages seen so far
    last_seen: float | None = None      # time.time() of the last live message
    events: deque = field(default_factory=lambda: deque(maxlen=EVENTS_KEPT))
    publications: dict[str, deque] = field(default_factory=dict)

    @property
    def label(self) -> str:
        dev = self.info.get("device", {})
        return dev.get("serial") or dev.get("name") or self.device_id

    @property
    def groups(self) -> list[str]:
        return list(self.info.get("groups") or [])

    @property
    def type(self) -> str:
        return self.info.get("device", {}).get("type", "")

    def has_feature(self, feature: str) -> bool:
        return any(f.get("feature") == feature or f.get("instance") == feature
                   for f in self.info.get("features", []))


class Fleet:
    def __init__(self, broker: str, port: int = 1883, *, username: str = "", password: str = "",
                 registry: Registry | None = None, features: Features | None = None):
        self.broker, self.port = broker, port
        self.registry = registry
        self.features = features or Features()
        self.loggers: dict[str, LoggerState] = {}
        self.connected = False
        self._listeners: set[Callable[[dict], None]] = set()
        self._pending: dict[str, _Pending] = {}
        self._loop: asyncio.AbstractEventLoop | None = None
        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                   client_id=f"logger-master-{secrets.token_hex(3)}")
        if username:
            self._client.username_pw_set(username, password)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(1, 10)

    # ------------------------------------------------------------ lifecycle

    async def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._client.connect_async(self.broker, self.port, keepalive=30)
        self._client.loop_start()
        self._stale_task = asyncio.create_task(self._watch_stale())

    async def stop(self) -> None:
        self._stale_task.cancel()
        self._client.disconnect()
        self._client.loop_stop()
        if self.registry:
            self.registry.save(force=True)

    def listen(self, fn: Callable[[dict], None]) -> Callable[[], None]:
        """fn(message) for every change; returns an unsubscribe function."""
        self._listeners.add(fn)
        return lambda: self._listeners.discard(fn)

    def _emit(self, msg: dict) -> None:
        for fn in list(self._listeners):
            try:
                fn(msg)
            except Exception:
                log.exception("listener failed")

    # ------------------------------------------------------------ MQTT (paho thread)

    def _on_connect(self, client, userdata, flags, reason, properties=None):
        if reason.is_failure:
            log.warning("broker refused connection: %s", reason)
            return
        for t in ("info", "status", "config", "events", "data/#", "cmd/ack"):
            client.subscribe(f"loggers/+/{t}", qos=1)
        self._call(self._set_connected, True)

    def _on_disconnect(self, client, userdata, flags, reason, properties=None):
        self._call(self._set_connected, False)

    def _on_message(self, client, userdata, msg):
        self._call(self._handle, msg.topic, bytes(msg.payload), bool(msg.retain))

    def _call(self, fn, *args):
        if self._loop and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(fn, *args)

    # ------------------------------------------------------------ state (asyncio loop)

    def _set_connected(self, up: bool) -> None:
        self.connected = up
        log.info("broker %s", "connected" if up else "disconnected")
        self._emit({"type": "broker", "connected": up})

    def _handle(self, topic: str, payload: bytes, retained: bool) -> None:
        parts = topic.split("/")
        if len(parts) < 3 or parts[0] != "loggers" or parts[1] in ("group", "all"):
            return
        dev_id, kind = parts[1], "/".join(parts[2:])
        if not payload:  # retained message cleared
            return
        try:
            body = json.loads(payload)
        except ValueError:
            log.warning("bad JSON on %s", topic)
            return
        if not isinstance(body, dict):
            return
        lg = self.loggers.get(dev_id)
        if lg is None:
            lg = self.loggers[dev_id] = LoggerState(dev_id)
        was_online = lg.online
        if kind == "status" and body.get("state") == "offline":
            lg.online = False
            lg.status = {**lg.status, "state": "offline"}
        elif not retained:
            lg.online = True
            lg.last_seen = time.time()

        if kind == "info":
            lg.info = body
            if self.registry:
                self.registry.seen(dev_id, info=body)
        elif kind == "status":
            if body.get("state") != "offline":
                lg.status = body
            if self.registry and not retained:
                self.registry.seen(dev_id, status=body)
        elif kind == "config":
            lg.config = body
        elif kind == "events":
            body.setdefault("received", time.time())
            lg.events.append(body)
            self._emit({"type": "event", "device_id": dev_id, "event": body})
        elif kind.startswith("data/"):
            name = kind[5:]
            q = lg.publications.get(name)
            if q is None:
                interval = max(float(body.get("window", {}).get("duration_s") or 1), 0.1)
                q = lg.publications[name] = deque(maxlen=int(PUBLICATION_SECONDS / interval) + 1)
            body["received"] = time.time()
            q.append(body)
            self._emit({"type": "data", "device_id": dev_id, "name": name, "message": body})
            if was_online == lg.online:
                return  # data is frequent: only push the logger summary when something changed
        elif kind == "cmd/ack":
            p = self._pending.get(str(body.get("id")))
            if p:
                p.add(dev_id, body)
            return
        self._emit({"type": "logger", "logger": self.summary(lg)})

    async def _watch_stale(self) -> None:
        while True:
            await asyncio.sleep(5)
            now = time.time()
            for lg in self.loggers.values():
                if lg.online and lg.last_seen and now - lg.last_seen > STALE_S:
                    lg.online = False
                    self._emit({"type": "logger", "logger": self.summary(lg)})

    # ------------------------------------------------------------ views

    def summary(self, lg: LoggerState) -> dict:
        reg = self.registry.get(lg.device_id) if self.registry else None
        dev = lg.info.get("device", {})
        return {
            "device_id": lg.device_id,
            "label": lg.label,
            "type": lg.type,
            # The logger's own report wins; the registry fills in for older firmware
            "model": dev.get("model") or (reg or {}).get("model") or "",
            "hardware_rev": dev.get("hardware_rev") or (reg or {}).get("hardware_rev") or "",
            "groups": lg.groups,
            "online": lg.online,
            "last_seen": lg.last_seen,
            "info": lg.info,
            "status": lg.status,
            "config": lg.config,
            "registry": reg,
            "mismatches": self.registry.mismatches(lg.device_id, lg.info) if self.registry else [],
            "publications": sorted(lg.publications),
        }

    def detail(self, dev_id: str) -> dict | None:
        lg = self.loggers.get(dev_id)
        if lg is None:
            return None
        return {**self.summary(lg), "events": list(lg.events),
                "publication_data": {k: list(v) for k, v in lg.publications.items()}}

    # ------------------------------------------------------------ commands

    def select(self, selection: dict) -> list[LoggerState]:
        """Loggers matching {ids: [...]} | {group: g} | {all: true}, narrowed by type/feature."""
        if selection.get("ids"):
            chosen = [self.loggers[i] for i in selection["ids"] if i in self.loggers]
        elif selection.get("group"):
            chosen = [lg for lg in self.loggers.values() if selection["group"] in lg.groups]
        elif selection.get("all"):
            chosen = list(self.loggers.values())
        else:
            raise ValueError("selection needs ids, group or all")
        if selection.get("type"):
            chosen = [lg for lg in chosen if lg.type == selection["type"]]
        if selection.get("feature"):
            chosen = [lg for lg in chosen if lg.has_feature(selection["feature"])]
        return chosen

    async def command(self, selection: dict, cmd: str, target: str | None = None,
                      args: dict | None = None, timeout: float = 8.0) -> dict:
        """Send a command and collect every logger's ack into one result (MQTT.md, Fleet operations)."""
        chosen = self.select(selection)
        cid = secrets.token_hex(4)
        payload = {"v": 1, "id": cid, "cmd": cmd}
        if target:
            payload["target"] = target
        if args:
            payload["args"] = args
        reachable = [lg for lg in chosen if lg.online is not False]
        pending = _Pending({lg.device_id for lg in reachable})
        self._pending[cid] = pending
        data = json.dumps(payload)
        narrowed = selection.get("type") or selection.get("feature")
        try:
            if selection.get("group") and not narrowed:
                self._client.publish(f"loggers/group/{selection['group']}/cmd", data, qos=1)
            elif selection.get("all") and not narrowed:
                self._client.publish("loggers/all/cmd", data, qos=1)
            else:
                for lg in reachable:
                    self._client.publish(f"loggers/{lg.device_id}/cmd", data, qos=1)
            if pending.expected:
                try:
                    await asyncio.wait_for(pending.done.wait(), timeout)
                except asyncio.TimeoutError:
                    pass
            if not pending.expected or pending.done.is_set():
                await asyncio.sleep(0.3 if not selection.get("ids") else 0)  # stragglers outside the known fleet
        finally:
            self._pending.pop(cid, None)
        return aggregate(cmd, chosen, pending.acks, {lg.device_id: lg.label for lg in self.loggers.values()},
                         cid)

    async def set_config(self, dev_id: str, changes: dict, timeout: float = 8.0) -> dict:
        lg = self.loggers.get(dev_id)
        if lg is None:
            raise KeyError(dev_id)
        errors, restart = self.features.validate_changes(lg.info, changes)
        if errors:
            return {"ok": False, "errors": errors, "summary": "not sent: " + "; ".join(errors)}
        res = await self.command({"ids": [dev_id]}, "set_config", args={"changes": changes}, timeout=timeout)
        r = res["results"][0] if res["results"] else {}
        res["restart_required"] = bool((r.get("result") or {}).get("restart_required")) or bool(restart)
        res["ok"] = r.get("status") == "ok"
        return res

    async def set_clocks(self, selection: dict) -> dict:
        """Fallback when NTP isn't working: set loggers' clocks to the master's UTC."""
        return await self.command(selection, "set_time", args={"utc_ms": int(time.time() * 1000)})


class _Pending:
    def __init__(self, expected: set[str]):
        self.expected = expected
        self.acks: dict[str, dict] = {}
        self.done = asyncio.Event()

    def add(self, dev_id: str, ack: dict) -> None:
        self.acks[dev_id] = ack
        if self.expected and self.expected <= set(self.acks):
            self.done.set()


def aggregate(cmd: str, chosen: list[LoggerState], acks: dict[str, dict], labels: dict[str, str],
              cid: str = "") -> dict:
    """One combined result: per-logger outcome plus a readable summary."""
    results = []
    ids = [lg.device_id for lg in chosen] + [i for i in acks if i not in {lg.device_id for lg in chosen}]
    online = {lg.device_id: lg.online for lg in chosen}
    for dev_id in ids:
        ack = acks.get(dev_id)
        r = {"device_id": dev_id, "label": labels.get(dev_id, dev_id)}
        if ack is None:
            r["status"] = "offline" if online.get(dev_id) is False else "no_reply"
        elif ack.get("ok"):
            r["status"] = "ok"
            r["result"] = ack.get("result") or {}
        elif ack.get("error") == "unsupported":
            r.update(status="not_applicable", message=ack.get("message", ""))
        else:
            r.update(status="error", error=ack.get("error", "failed"), message=ack.get("message", ""))
        results.append(r)

    applicable = [r for r in results if r["status"] != "not_applicable"]
    ok = sum(r["status"] == "ok" for r in applicable)
    parts = [f"{cmd}: {ok} of {len(applicable)} ok"]
    for r in applicable:
        if r["status"] == "offline":
            parts.append(f"{r['label']} offline")
        elif r["status"] == "no_reply":
            parts.append(f"{r['label']} didn't reply")
        elif r["status"] == "error":
            parts.append(f"{r['label']} {r['error']}" + (f": {r['message']}" if r.get("message") else ""))
    na = len(results) - len(applicable)
    if na:
        parts.append(f"{na} without this command")
    return {"id": cid, "cmd": cmd, "results": results, "summary": "; ".join(parts),
            "ok": bool(applicable) and ok == len(applicable)}
