# Feature definitions

Every logger is built from **features**: self-contained capabilities with a fixed interface.
This folder is the single source of truth for those interfaces. Firmware implements them,
the master and dashboards read them to know what to expect and which controls to show, and the
Python reader uses them to interpret streams.

See [../MQTT.md](../MQTT.md#features-one-interface-per-capability) for how features appear in
`info`, `status`, `config` and commands.

## Features (v1)

| Feature | File | Multi-instance | Summary |
|---|---|:-:|---|
| `core` | [core.json](core.json) | | Every logger: identity, LED, time, MQTT, config and housekeeping commands |
| `recording` | [recording.json](recording.json) | | Auto-start, start/stop, file rolling, sinks |
| `sd` | [sd.json](sd.json) | | SD card storage |
| `battery` | [battery.json](battery.json) | | Battery level and charging |
| `wifi` | [wifi.json](wifi.json) | | Known networks, fallback hotspot |
| `accel` | [accel.json](accel.json) | ✓ | 3-axis accelerometer (ADXL355, ADXL345, simulated) |
| `env` | [env.json](env.json) | ✓ | Environment: temperature, humidity, pressure (BME280) |
| `publish` | [publish.json](publish.json) | | Configurable data publications over MQTT |

Planned: `ethernet`, `netstream` (DLOG over TCP), `tacho` (once-per-rev speed reference).

## Logger types

A logger type is a set of features plus defaults. Only `core` and `recording` are universal:
an Ethernet streaming logger, for example, has no `sd`, `battery` or `wifi` at all, and the
core never assumes them (config and identity live in internal flash). The portable vibration
logger is:

| Instance | Feature | Implementation | Required |
|---|---|---|:-:|
| `core` | core | | ✓ |
| `recording` | recording | | ✓ |
| `sd` | sd | Adafruit 4682 | ✓ |
| `battery` | battery | MAX17048 (on the TinyS3[D]) | ✓ |
| `wifi` | wifi | | ✓ |
| `accel` | accel | ADXL355 (`sim` for development) | ✓ |
| `env` | env | BME280 | if fitted |
| `publish` | publish | | ✓ |

## Definition format

Each file is one JSON object:

```json
{
  "feature": "accel",
  "version": 1,
  "multi_instance": true,
  "description": "…",
  "config":   { "<field>": { <field spec> } },
  "status":   { "<field>": { <field spec> } },
  "commands": { "<name>": { "description": "…", "args": { "<field>": { <field spec> } } } },
  "streams":  [ { <stream template> } ],
  "events":   { "<name>": "description" }
}
```

**Field spec:**

| Key | Meaning |
|---|---|
| `type` | `string`, `integer`, `number`, `boolean`, `array`, `object` |
| `default` | Value used when config omits the field |
| `enum` / `min` / `max` | Allowed values. A logger's `info` can narrow these further with `limits` |
| `unit` | Engineering unit, e.g. `s`, `g`, `Hz`, `%` |
| `items` | For arrays: the field spec of each element |
| `fields` | For objects: field specs of the members |
| `secret` | `true` for passwords etc.: never published, never written to DLOG metadata |
| `applies` | When a config change takes effect: `immediate` (default), `new_file` (rolls the data file), `restart` |
| `description` | Human text, shown by dashboards |

**Stream templates** describe the streams an instance produces. `{instance}` and
`{config.<field>}` are filled in from the instance name and its config.

**Implementation-specific config** goes in an `impl` object inside the instance's config (e.g.
the simulated sensor's waveform settings). It's passed to the implementation untouched and
isn't part of the shared interface.

## Rules for changing a feature

- Adding optional config/status fields, commands or events: same `version`.
- Renaming or removing anything, or changing its meaning or type: **new `version`**. Firmware
  reports the version it implements, and the master can support more than one.
- A new capability that doesn't fit an existing feature: a **new feature**, not a variant of an
  old one.
