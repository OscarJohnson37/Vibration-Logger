# master

Python tools for the master, an Ubuntu PC (master) at the site:

- `logger_reader`: reads DLOG files into pandas, groups them into sessions, captures a logger's
  USB stream.
- `logger_master`: the fleet service. Talks MQTT to every logger (protocol/MQTT.md), keeps the
  registry (`registry/devices.yaml`), and serves the dashboard. Includes a logger simulator.

Setting up the Ubuntu PC itself (hotspot, Mosquitto, mDNS, NTP, service): [deploy/ubuntu](../deploy/ubuntu/README.md).

## Setup

```powershell
cd master
pip install -e ".[server,capture,test]"   # numpy, pandas, paho-mqtt, fastapi, uvicorn, pyyaml, pyserial, pytest
python -m pytest                           # run the tests
```

## Dashboard

```powershell
python -m logger_master serve --broker localhost        # http://localhost:8080
```

| Option | Default | |
|---|---|---|
| `--broker`, `--mqtt-port` | `localhost`, 1883 | MQTT broker |
| `--mqtt-user`, `--mqtt-password` | `$LOGGER_MQTT_USER`, `$LOGGER_MQTT_PASSWORD` | Broker credentials |
| `--http-port`, `--host` | 8080, `0.0.0.0` | Where the dashboard is served |
| `--registry` | `registry/devices.yaml` in the repo | Device registry |
| `--features` | `protocol/features` in the repo | Feature definitions (also `$LOGGER_FEATURES_DIR`) |

What it does:

- **Fleet view:** every logger with online/offline, state, battery, recording and session, SD
  space, sensor health, WiFi signal, clock and location, plus registry mismatches (an expected
  feature missing, a different serial). Filter by group.
- **Fleet actions:** start (optionally into a named session), stop, identify, mark, sync time,
  or set clocks from the master, for ticked loggers, a group or everyone. Every logger's ack is
  collected into one result ("start: 6 of 7 ok; VL-0004 offline").
- **Per logger:** status of every feature, control and self-tests, a config editor generated from
  the feature definitions (values checked against the definitions and the logger's own limits
  before sending; restart prompts), registry fields, recent events, live plots of its data
  publications, and for accelerometer loggers a 3D **orientation** view from the gravity vector.
  Like tools/accel-viewer, it can show an uploaded model (STL, OBJ or GLB), turn it relative to
  the axes, and remap/invert which sensor axis drives each shown axis. The zero, axis map, model
  turn and model file are stored on the master per logger (`registry/views/`), so every browser
  shows the same thing.
- An **About** tab shows the logger's model page from the model catalogue
  (`registry/models/<model>.md`: specs, LED guide, instructions, pin map, troubleshooting). The
  model number comes from the logger (`info.device.model`, e.g. `VL-P1`) or its registry entry;
  the model's `expected_features` are what each unit is checked against unless its registry
  entry lists its own. Pages are re-read when edited. `GET /api/models`, `/api/models/<model>`.
- Live data and orientation come from the loggers' MQTT publications (e.g. `accel_1s`), which
  the vibration logger sends whether or not it's recording.
- It works with no internet: everything it needs (including three.js) is served locally.

REST API (used by the dashboard): `GET /api/fleet`, `GET /api/loggers/{id}`,
`POST /api/command` `{selection: {ids|group|all, type?, feature?}, cmd, target?, args?}`,
`POST /api/loggers/{id}/config` `{changes}`, `PUT /api/registry/{id}`,
`POST /api/time/loggers` `{selection}`, `GET /api/features`, and `/ws` for live updates.

## Developing without hardware

On a PC without Mosquitto, run a pure-Python broker, some simulated loggers and the dashboard
in three terminals:

```powershell
pip install -e ".[server,dev-broker]"
python -m logger_master broker                       # development broker on 1883 (amqtt)
python -m logger_master simulate --count 3           # SIM-0001..3: record, publish, answer commands
python -m logger_master serve                        # http://localhost:8080
```

The simulated loggers behave like the firmware as far as MQTT goes: retained info/status/config
with a last-will, auto-start after 10 s, sessions, acks for every command (including
`set_config` validation and `busy`), an `accel_1s` stats publication with a slowly tilting
board, a draining battery, and one logger with an unset clock.

## Reading data

```python
from logger_reader import find_sessions, read_dlog, read_session

for s in find_sessions("D:/logs"):            # SD card contents, or files from many loggers
    print(s.name, s.logger, s.boots)          # session name, serial, boots it spans
df = s.to_dataframe("accel")                  # all boots, with a 'boot' column

f = read_dlog("logs/pump2-baseline/VL-0001_b000042_f0003.dlog")
print(f.summary())                     # device, streams, gaps, clipping, CRC errors…
f.session_name                         # "pump2-baseline"
df = f.to_dataframe("accel")           # UTC DatetimeIndex if the clock was synced
df = f.to_dataframe("accel", mounted=True)   # apply the mounting axis map
df.attrs["units"], df.attrs["gaps"]

# One boot's files (one recording split by file rolling):
s = read_session(sorted(Path("logs/pump2-baseline").glob("VL-0001_b000042_*.dlog")))
df = s.to_dataframe("accel")
```

Files hold raw counts; `to_dataframe` applies scaling (`scaled=False` for raw counts), the
optional mounting axis map, and the time model from FILE_FORMAT.md (time syncs applied
backwards and interpolated for drift). Both file layouts are read: v1.1
(`/logs/<session>/<logger>_b…_f….dlog`) and v1.0 (`/logs/<device id>/b…_f….dlog`, no session).

## Command line

```powershell
python -m logger_reader info FILE [FILE ...]                 # summaries
python -m logger_reader export FILE --stream accel --csv out.csv
python -m logger_reader capture COM22 test.dlog --seconds 30  # record a logger's USB stream
```

`capture` needs the logger to be recording with the `usb` sink. Close any serial monitor first.
