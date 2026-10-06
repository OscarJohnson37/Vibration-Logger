# MQTT control protocol, v1

Status: draft v1, 2026-10-06

How loggers and the master PC talk: status, commands and configuration. **Raw sample data
never goes over MQTT.** It's recorded to SD, or streamed as DLOG over TCP (see
[FILE_FORMAT.md](FILE_FORMAT.md#network-streaming)), so a busy data stream can't delay control
messages.

## Network

- The master hosts a private network (WiFi access point; Ethernet loggers on the same LAN) and
  runs the **MQTT broker** (Mosquitto, port 1883), an **NTP server**, and the fleet service.
- Loggers find the broker from their config (`mqtt.host`), falling back to mDNS
  (`_mqtt._tcp.local`). No IP addresses are hard-coded.
- Each logger advertises itself over mDNS as `logger-<device_id>.local`.
- Every logger must work with no network at all. MQTT is for monitoring and control, never a
  requirement for recording.

## Conventions

- Topic root: `loggers/<device_id>/…`, where `device_id` is the 12-digit lowercase hex MAC,
  the same as `device.id` in DLOG files.
- Payloads are UTF-8 JSON objects, and every one includes `"v": 1` (this protocol's version).
- Times: `t` is UTC milliseconds since the Unix epoch, or `null` if the clock isn't synced;
  `uptime_ms` is always included alongside it.
- Receivers ignore fields they don't recognise.
- MQTT client ID: `logger-<device_id>`.

## Topics

| Topic | Direction | QoS | Retained | Purpose |
|---|---|:-:|:-:|---|
| `loggers/<id>/info` | logger → master | 1 | ✓ | What the logger is: identity, streams, capabilities |
| `loggers/<id>/status` | logger → master | 1 | ✓ | How it's doing right now. Last-will: `offline` |
| `loggers/<id>/config` | logger → master | 1 | ✓ | Its current configuration (secrets removed) |
| `loggers/<id>/events` | logger → master | 1 | | Notable things as they happen |
| `loggers/<id>/live/<stream>` | logger → master | 0 | | Optional low-rate summaries for field checks |
| `loggers/<id>/cmd` | master → logger | 1 | | Commands to one logger |
| `loggers/all/cmd` | master → all | 1 | | Commands to every logger |
| `loggers/<id>/cmd/ack` | logger → master | 1 | | Command results |

Retained topics mean the master sees every logger's latest info, status and config as soon as
it connects, including the ones that are currently offline.

## info

Published on connect, and whenever it changes. The `streams` array uses the same schema as the
DLOG metadata, so the master knows exactly what a logger records.

```json
{
  "v": 1,
  "device": { "id": "9070692f3338", "name": "vib-01", "type": "vibration",
              "hardware": "TinyS3[D]", "firmware": "0.2.0" },
  "streams": [ { "id": 1, "name": "accel", "rate_hz": 4000, "dtype": "i24", "channels": [] } ],
  "sinks": ["sd"],
  "capabilities": ["start_stop", "identify", "mark", "set_config", "live"],
  "network": { "interface": "wifi", "ip": "10.0.0.23", "mac": "90:70:69:2f:33:38" }
}
```

## status

Published every **30 s** (configurable), and immediately when anything important changes.

```json
{
  "v": 1,
  "t": 1791270123456,
  "uptime_ms": 3723000,
  "state": "recording",
  "boot_id": 42,
  "battery": { "voltage": 3.92, "percent": 81, "charging": false, "usb_power": false },
  "sd": { "present": true, "free_mb": 28311, "total_mb": 30436, "file": "b000042_f0003.dlog" },
  "time": { "synced": true, "source": "ntp", "last_sync_age_s": 312, "uncertainty_ms": 5 },
  "streams": [
    { "name": "accel", "ok": true, "frames": 14892000, "gaps": 0, "clipped_blocks": 2, "errors": 0 }
  ],
  "temperature_c": 31.5,
  "wifi_rssi_dbm": -58,
  "errors": []
}
```

| state | Meaning |
|---|---|
| `idle` | Powered and connected, not recording |
| `recording` | Writing data to at least one sink |
| `error` | Can't record (no SD card, sensor missing, …); see `errors` |
| `sleeping` | Battery duty cycle: about to sleep, will report again at the next wake |
| `offline` | Set by the broker (last-will) when the logger disconnects unexpectedly |

On a clean disconnect, such as going to sleep, the logger publishes `sleeping` itself, so the
master can tell "asleep as planned" apart from "lost".

## config

The logger's full current configuration, minus secrets such as WiFi passwords and MQTT
credentials. Published on connect and after every change. The configuration schema itself is
defined with the core library (phase 2); this topic just reports it.

## events

One message per event, the same names as DLOG `event` markers, so the SD file and the master
agree on what happened.

```json
{ "v": 1, "t": 1791270123456, "uptime_ms": 3723000, "name": "battery_low",
  "detail": { "percent": 9 } }
```

Defined events: `battery_low`, `temp_high`, `sd_full`, `sd_error`, `sensor_error`, `clipping`,
`time_synced`, `file_rolled`, `recording_started`, `recording_stopped`.

## live/\<stream\>

Optional, off by default. Small summaries so you can check sensors in the field without
retrieving the card; never raw samples.

```json
{ "v": 1, "t": 1791270123456, "window_s": 1.0,
  "channels": ["x", "y", "z"], "unit": "g",
  "rms": [0.012, 0.009, 0.031], "peak": [0.051, 0.040, 0.112] }
```

## Commands

Sent to `loggers/<id>/cmd`, or to `loggers/all/cmd` for every logger.

```json
{ "v": 1, "id": "c1b2", "cmd": "mark", "args": { "text": "pump started" } }
```

`id` is chosen by the sender, so it can match the reply. Every command gets exactly one reply on
`loggers/<id>/cmd/ack`:

```json
{ "v": 1, "id": "c1b2", "ok": true, "result": {} }
{ "v": 1, "id": "c1b3", "ok": false, "error": "no_sd_card", "message": "SD card not present" }
```

| cmd | args | Effect |
|---|---|---|
| `start` | | Start recording (opens a new file) |
| `stop` | | Stop recording (writes END, closes the file) |
| `mark` | `text` | Write a `note` marker into the data file. For noting what a machine was doing |
| `identify` | `seconds` (default 10) | Flash the status LED so you can find this logger |
| `get_config` | | Re-publish `config` |
| `set_config` | `changes` (partial config object) | Apply settings. Changes that alter streams roll to a new file. `result.restart_required` if a reboot is needed |
| `sync_time` | | Sync with the NTP server now |
| `roll_file` | | Close the current file and open the next |
| `reboot` | | Restart the logger (acks first) |

Commands a logger doesn't support get `ok: false, error: "unsupported"`. Commands received while
asleep are lost; the master resends them after the logger's next `status`.

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
| 1 | 2026-10-06 | First version |
