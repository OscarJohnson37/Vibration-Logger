# MQTT control protocol, v1

Status: draft v1, updated 2026-10-08 (feature model, groups, data publications)

How loggers and the master PC talk: identity, status, commands, configuration, and configurable
data publications (live values, statistics, short snapshots). **Continuous full-rate data never
goes over MQTT.** It's recorded to SD, or streamed as DLOG over TCP (see
[FILE_FORMAT.md](FILE_FORMAT.md#network-streaming)), so a busy data stream can't delay control
messages.

## Features: one interface per capability

Loggers differ (different sensors, storage, power), but anything they have in common must be
controlled the same way. So every logger is built from **features**: self-contained
capabilities with a fixed interface: config fields, status fields, commands, streams and
events. A logger is just the set of features it has; two loggers with the same feature are
controlled identically.

- Features are defined in [features/](features/), one file each, and versioned.
- A feature can appear more than once on a logger (e.g. two accelerometers). Each occurrence is
  an **instance** with its own name (`accel_de`, `accel_nde`). A single instance is usually
  named after the feature.
- Every logger has `core`. Everything else is optional.
- The feature is the *interface*, not the hardware: an ADXL355, an ADXL345 and the simulated
  sensor all implement `accel`, and report which hardware they are in `impl`.

## Network

- The master hosts a private network (a dedicated WiFi router/access point, with Ethernet
  loggers on the same LAN) and runs the **MQTT broker** (Mosquitto, port 1883), an **NTP
  server**, and the fleet service.
- Loggers get network settings from their config (see the `wifi` feature) and find the broker
  from config (`core.mqtt_host`), falling back to mDNS (`_mqtt._tcp.local`). No IP addresses are
  hard-coded.
- Each logger advertises itself over mDNS as `logger-<device_id>.local`.
- Every logger must work with no network at all. MQTT is for monitoring and control, never a
  requirement for recording.

## Conventions

- `device_id` is the 12-digit lowercase hex MAC: permanent, unique, the same as `device.id` in
  DLOG files. `serial` is the human-readable label on the case (e.g. `VL-0001`), assigned when
  the unit is built.
- Topic root: `loggers/<device_id>/…`.
- Payloads are UTF-8 JSON objects, and every one includes `"v": 1` (this protocol's version).
- Times: `t` is UTC milliseconds since the Unix epoch, or `null` if the clock isn't synced;
  `uptime_ms` is always included alongside it.
- Receivers ignore fields they don't recognise.
- MQTT client ID: `logger-<device_id>`.

## Topics

| Topic | Direction | QoS | Retained | Purpose |
|---|---|:-:|:-:|---|
| `loggers/<id>/info` | logger → master | 1 | ✓ | What the logger is: identity, features, streams |
| `loggers/<id>/status` | logger → master | 1 | ✓ | How it's doing right now. Last-will: `offline` |
| `loggers/<id>/config` | logger → master | 1 | ✓ | Its current configuration (secrets removed) |
| `loggers/<id>/events` | logger → master | 1 | | Notable things as they happen |
| `loggers/<id>/data/<publication>` | logger → master | 0* | | Configurable data publications |
| `loggers/<id>/cmd` | master → logger | 1 | | Commands to one logger |
| `loggers/group/<group>/cmd` | master → group | 1 | | Commands to every logger in a group |
| `loggers/all/cmd` | master → all | 1 | | Commands to every logger |
| `loggers/<id>/cmd/ack` | logger → master | 1 | | Command results (always per logger) |

\* QoS and retain are set per publication.

Retained topics mean the master sees every logger's latest info, status and config as soon as
it connects, including the ones that are currently offline.

## info

Published on connect, and whenever it changes. Tells the master everything about what this
logger can do: its features (with versions and instances) and its streams (the same schema as
the DLOG metadata).

```json
{
  "v": 1,
  "device": { "id": "9070692f3338", "serial": "VL-0001", "name": "vib-01", "type": "vibration",
              "model": "VL-P1", "hardware_rev": "A", "hardware": "TinyS3[D]", "firmware": "0.2.0" },
  "features": [
    { "instance": "core",      "feature": "core",      "version": 1 },
    { "instance": "recording", "feature": "recording", "version": 1 },
    { "instance": "sd",        "feature": "sd",        "version": 1 },
    { "instance": "battery",   "feature": "battery",   "version": 1, "impl": "MAX17048" },
    { "instance": "wifi",      "feature": "wifi",      "version": 1 },
    { "instance": "accel",     "feature": "accel",     "version": 1, "impl": "ADXL355",
      "limits": { "range_g": [2, 4, 8], "odr_hz": [125, 250, 500, 1000, 2000, 4000] } },
    { "instance": "publish",   "feature": "publish",   "version": 1,
      "methods": ["latest", "mean", "stats", "snapshot"] }
  ],
  "streams": [ { "id": 1, "name": "accel", "feature": "accel", "rate_hz": 4000, "dtype": "i24",
                 "channels": [] } ],
  "groups": ["site-a", "pump-house"],
  "network": { "interface": "wifi", "ip": "10.0.0.23", "mac": "90:70:69:2f:33:38" }
}
```

- `device.model` and `device.hardware_rev` are the product model number and revision (e.g. `VL-P1`
  rev `A`); the master looks the model up in its catalogue (`registry/models/<model>.md`) for the
  model's documentation and expected features.
- `impl` names the hardware behind a feature instance, for information only.
- `limits` narrows a feature's allowed config values to what this implementation supports
  (e.g. the ADXL355 has no ±16 g range). `set_config` outside these limits is rejected.
- Feature-specific facts (like `publish`'s `methods`) can be added to the feature's entry.

## status

Published every **30 s** (configurable), and immediately when anything important changes. A
few summary fields at the top level, then each feature instance's status under its instance
name, in the shape its feature definition specifies.

```json
{
  "v": 1,
  "t": 1791270123456,
  "uptime_ms": 3723000,
  "state": "recording",
  "boot_id": 42,
  "errors": [],
  "features": {
    "core":      { "temperature_c": 31.5, "time": { "synced": true, "source": "ntp", "last_sync_age_s": 312 } },
    "recording": { "recording": true, "file": "b000042_f0003.dlog", "started_ms": 1791266400000 },
    "sd":        { "present": true, "free_mb": 28311, "total_mb": 30436 },
    "battery":   { "voltage": 3.92, "percent": 81, "level": "high", "charging": false, "usb_power": false, "full": false },
    "wifi":      { "ssid": "logger-net", "rssi_dbm": -58 },
    "accel":     { "ok": true, "frames": 14892000, "gaps": 0, "clipped_blocks": 2, "errors": 0 },
    "publish":   { "publications": [ { "name": "accel_1s", "sent": 3712, "skipped": 0 } ] }
  }
}
```

| state | Meaning |
|---|---|
| `starting` | Counting down to auto-start, or still initialising |
| `idle` | Powered, not recording |
| `recording` | Writing data to at least one sink |
| `error` | Can't record (no SD card, sensor missing, …); see `errors` |
| `sleeping` | Battery duty cycle: about to sleep, will report again at the next wake |
| `offline` | Set by the broker (last-will) when the logger disconnects unexpectedly |

On a clean disconnect, such as going to sleep, the logger publishes `sleeping` itself, so the
master can tell "asleep as planned" apart from "lost".

## config

The logger's full configuration, one section per feature instance, in the shape each feature
definition specifies. Secrets (WiFi passwords, MQTT credentials) are removed. Published on
connect and after every change.

```json
{
  "v": 1,
  "core":      { "name": "vib-01", "groups": ["site-a"], "mqtt_host": "", "status_interval_s": 30 },
  "recording": { "autostart": true, "start_delay_s": 10, "flush_interval_s": 5,
                 "file_max_minutes": 60, "file_max_mb": 256 },
  "accel":     { "range_g": 8, "odr_hz": 4000,
                 "mounting": { "axis_map": [[1,0,0],[0,1,0],[0,0,1]], "location": "motor DE" } },
  "publish":   { "budget_kbps": 32,
                 "publications": [ { "name": "accel_1s", "stream": "accel", "method": "stats", "interval_s": 1 } ] }
}
```

On the device the config (with secrets) is stored in internal flash, so loggers without an SD
card have it too. Loggers with a card also keep it as `config.json` on the card, where it can
be edited on a PC (applied at the next boot). Missing sections and fields take the logger
type's defaults, then the defaults from the feature definitions.

## events

One message per event, with the same names as DLOG `event` markers, so the SD file and the
master agree on what happened. `source` is the feature instance that raised it.

```json
{ "v": 1, "t": 1791270123456, "uptime_ms": 3723000, "name": "battery_low", "source": "battery",
  "detail": { "percent": 29 } }
```

Each feature definition lists the events it can raise.

## Commands

Sent to one logger (`loggers/<id>/cmd`), a group (`loggers/group/<group>/cmd`) or everyone
(`loggers/all/cmd`). The payload is the same in all three cases:

```json
{ "v": 1, "id": "c1b2", "cmd": "self_test", "target": "accel_de", "args": {} }
```

- `cmd` is defined by a feature; `target` is the feature instance to run it on.
- **`target` can be left out** when exactly one instance on the logger has that command. That
  covers nearly everything (`start`, `stop`, `mark`, `identify`, …), so everyday commands stay
  short: `{ "v": 1, "id": "c1", "cmd": "start" }`.
- `id` is chosen by the sender to match the replies.

Every logger that receives a command replies exactly once, on its own `loggers/<id>/cmd/ack`:

```json
{ "v": 1, "id": "c1b2", "ok": true, "result": {} }
{ "v": 1, "id": "c1b3", "ok": false, "error": "unsupported", "message": "No feature provides 'self_test'" }
```

| error | Meaning |
|---|---|
| `unsupported` | No feature instance on this logger provides the command (normal for group/all commands) |
| `unknown_target` | `target` names an instance this logger doesn't have |
| `ambiguous_target` | `target` was left out, but more than one instance has this command |
| `invalid_args` | Arguments missing, wrong type, or outside the feature's limits |
| `busy` | Can't run it right now (e.g. `self_test` while recording); try later |
| `failed` | It ran and failed; `message` says why |

The commands themselves are defined per feature (see [features/](features/)): e.g. `core`
provides `identify`, `mark`, `get_config`, `set_config`, `sync_time`, `reboot`; `recording`
provides `start`, `stop`, `roll_file`; `publish` provides `publish`; `accel` provides
`self_test`.

Commands received while asleep are lost; the master resends them after the logger's next
`status`.

## Groups and fleet operations

- A logger's groups are set in its config (`core.groups`) and reported in `info`. It subscribes
  to `loggers/group/<group>/cmd` for each.
- **Fleet operations happen on the master.** "Start every vibration logger in the pump house",
  or "self-test every accelerometer", is resolved by the fleet service: it picks loggers by
  group, type or feature (from their retained `info`), sends the command, and collects each
  logger's ack into one combined result ("started 6 of 7; VL-0004 offline").
- Loggers never need to know about each other.

## Data publications: data/\<publication\>

Provided by the `publish` feature. A logger can publish its recent data over MQTT while it
records, in whatever form is useful: the latest frame, a one-second average, vibration
statistics, a short raw snapshot, and later anything else. Nothing about the shape is fixed by
the protocol:

- Each **publication** is defined in the logger's config: which stream, which **method**
  turns the recent samples into a message, how often, and over how much data.
- A logger can run **any number** of publications at once, each on its own topic.
- Every message **describes itself** (method, window, channels, units), so the master can
  store and display methods it has never seen before.
- New methods are added in firmware by name; the protocol doesn't change.

Publications are a **live view, not the record**. They're best-effort: sent only while
connected, never queued while offline, and dropped if they'd exceed the logger's publishing
budget. The full-rate data always goes to the logger's recording sinks (SD card and/or the
network stream).

### Defining a publication

In `config.publish.publications` (so set with `set_config`), or sent once with the `publish`
command:

```json
{
  "name": "accel_1s",
  "stream": "accel",
  "method": "stats",
  "params": { "stats": ["mean", "ac_rms", "peak"] },
  "interval_s": 1,
  "window_s": 1,
  "channels": ["x", "y", "z"],
  "axes": "mounted",
  "qos": 0,
  "enabled": true
}
```

| Field | Default | Meaning |
|---|---|---|
| `name` | required | Topic suffix: `loggers/<id>/data/<name>`. Letters, digits, `_`, `-` |
| `stream` | required | Source stream, as named in `info` |
| `method` | required | How samples become a message; see methods below |
| `params` | `{}` | Method-specific options |
| `interval_s` | `1` | How often to publish |
| `window_s` | `interval_s` | How much recent data each message covers. Can be shorter (a sample of each interval) or longer (overlapping windows) |
| `channels` | all | Subset of the stream's channels |
| `axes` | `"sensor"` | `"sensor"`: the sensor's own axes, like the stored data. `"mounted"`: after the stream's mounting `axis_map` |
| `qos` | `0` | MQTT QoS. `1` for publications you don't want to miss while connected |
| `retain` | `false` | Retain the latest message, so a dashboard sees it immediately on connecting |
| `enabled` | `true` | |

Values are always in **engineering units** (the channel's `scale` and `offset` applied).

### Methods (v1)

| method | params | `values` in each message |
|---|---|---|
| `latest` | | `latest`: the most recent frame |
| `mean` | | `mean`: average over the window |
| `stats` | `stats`: any of `mean`, `min`, `max`, `rms`, `ac_rms`, `peak`, `peak_to_peak`, `crest` (default `mean`, `ac_rms`, `peak`) | One array per requested statistic. `ac_rms` = RMS with the mean removed (vibration level); `peak` = largest deviation from the mean; `crest` = peak / ac_rms |
| `snapshot` | `frames` (default 256, max 4096), `decimate` (keep every n-th frame, default 1) | Raw samples, in `data` instead of `values` (see below) |

Planned, not in v1: `spectrum` (FFT magnitudes or band levels), `velocity` (vibration velocity
in mm/s, ISO band), and threshold triggers (publish only when a value crosses a limit).
Experimental methods use an `x_` prefix, e.g. `x_bearing_env`. Each logger lists the methods it
supports in its `publish` entry in `info`.

### Message format

```json
{
  "v": 1,
  "t": 1791270123456,
  "uptime_ms": 3723000,
  "name": "accel_1s",
  "stream": "accel",
  "method": "stats",
  "params": { "stats": ["mean", "ac_rms", "peak"] },
  "window": { "t_mono_us": 3722000000, "duration_s": 1.0, "frames": 4000, "flags": [] },
  "channels": ["x", "y", "z"],
  "unit": "g",
  "axes": "mounted",
  "values": {
    "mean":   [0.002, -0.011, 0.998],
    "ac_rms": [0.012, 0.009, 0.031],
    "peak":   [0.051, 0.040, 0.112]
  }
}
```

- `values` maps each result name to an array with one entry per channel, in `channels` order.
  A receiver that doesn't know the method can still store and plot it.
- `window.flags` lists anything that affects the data in the window: `clipped`, `gap`, `suspect`
  (the same meanings as the DLOG block flags).
- `window.t_mono_us` is the first frame's device time, so publications line up exactly with the
  SD data.

**Snapshots** carry raw samples as a DLOG DATA record (see
[FILE_FORMAT.md](FILE_FORMAT.md#data-records)), base64-encoded, so the same decoder and the
stream metadata from `info` interpret them:

```json
{ "v": 1, "t": 1791270123456, "uptime_ms": 3723000, "name": "accel_wave", "stream": "accel",
  "method": "snapshot", "params": { "frames": 512, "decimate": 1 },
  "window": { "t_mono_us": 3722870000, "duration_s": 0.128, "frames": 512, "flags": [] },
  "channels": ["x", "y", "z"], "axes": "sensor",
  "data": { "encoding": "dlog_b64", "record": "AQAAAAACAAAAAAAA…" } }
```

### Limits

- Message size: at most **16 KB**. Larger snapshots are refused in config validation.
- Publishing budget: total publication traffic per logger is capped (`publish.budget_kbps`,
  default 32 kB/s). Messages over budget are skipped and counted in the `publish` status, so
  publishing can never starve recording or control messages.

## Time sync

Loggers sync with the master's NTP server when they connect, then every hour (configurable),
and on `sync_time`. Every successful sync also writes a `time_sync` marker into the current
data file, which is what lets the reader correct timestamps afterwards.

## Security

v1 relies on the master's private network: the broker requires a username and password
(per site, in each logger's config), and the network isn't connected to the internet. TLS can
be added in a later version without changing topics or payloads.

## Changes

| Version | Date | Changes |
|---|---|---|
| 1 | 2026-10-08 | First version: feature model, groups, configurable data publications |
