# Logger Ecosystem: Architecture Plan

Status: draft, updated 2026-10-08 (feature model added; phase 2 in progress)

## Goals

A family of data loggers sharing one codebase. Each logger type (vibration first, others later)
adds only its own sensor code; everything else is shared:

- **Common data format.** Every logger writes the same self-describing file format. One Python
  reader loads any logger's data into pandas.
- **Common network protocol.** Every logger talks to the master PC the same way, over WiFi now
  and Ethernet later.
- **Common device behaviour.** Identity, configuration, timekeeping, status reporting and power
  modes work the same everywhere.

## Scope decisions

| Topic     | Decision                                                                            | Consequence                                                                          |
| --------- | ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| Data path | **Pluggable sinks**: SD card, network stream, or both, chosen per logger      | Not locked into SD-only; the same block format is used for files and network streams |
| Data type | High-rate raw waveforms (plus slow channels)                                        | Compact binary format, not CSV                                                       |
| Master    | MQTT broker (Mosquitto) + Python service                                            | Same protocol over WiFi or Ethernet                                                  |
| Network   | Master PC hosts its own WiFi access point; some loggers on Ethernet to the same LAN | Private, offline network; master provides DHCP, NTP, MQTT; no internet dependency    |
| Scale     | Many loggers, mixed WiFi/Ethernet                                                   | Unique device IDs, fleet tooling, no hard-coded IPs                                  |
| Time sync | Master sets the time; high accuracy not needed                                      | Sync whenever connected; RTC is an optional add-on                                   |
| Hardware  | ESP32 family only                                                                   | One C++ codebase, one PlatformIO project                                             |
| Power     | Mix of mains and battery                                                            | Core supports an always-on profile and a duty-cycled profile                         |
| Analysis  | Python (pandas/numpy)                                                               | Python reader package is a first-class deliverable                                   |

## System overview

```
 ┌─────────────────── Logger (ESP32) ───────────────────┐
 │                                                      │
 │  Sensor modules ──► Ring buffer ──► Sink dispatcher  │
 │  (vibration, …)       (PSRAM)        │         │     │
 │                                      ▼         ▼     │
 │                                  SD sink   Net sink  │──── data blocks (optional) ───┐
 │                                                      │                               │
 │  Core: config · time · status · commands · power     │◄─── MQTT control, NTP ───┐    │
 │        WiFi / Ethernet                               │                          │    │
 └──────────────────────────────────────────────────────┘                          │    │
                                                                                   │    ▼
       Master PC (hosts WiFi AP; Ethernet loggers on same LAN via switch)  ┌───────────────────┐
         DHCP · NTP · Mosquitto · fleet service · stream receiver  ◄───────┤  private network  │
                                                                           └───────────────────┘
       SD card retrieved ──► Python reader ──► pandas DataFrame
       Stream receiver  ──► same file format on disk ──► same Python reader
```

The stream receiver on the master writes incoming blocks into the **same file format** as the SD
card. Analysis code never needs to know how the data arrived.

## Logger variants this supports

| Variant                                        | Sinks                                | Network use                                                                                                                          | Power            |
| ---------------------------------------------- | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------ | ---------------- |
| **Portable vibration logger (this one)** | SD only                              | Usually connected to the master over WiFi for time sync, config, status and commands, but never required; data is not sent over WiFi | Battery          |
| Fixed streaming logger (future)                | Network only, or network + SD backup | Continuous over Ethernet                                                                                                             | Mains            |
| Remote monitor (future)                        | SD + low-rate summaries over network | Periodic                                                                                                                             | Battery or mains |

All three are the same firmware core with a different config and sensor set.

## Repository layout

One repo, one PlatformIO project, one build environment per logger type:

```
firmware/
  platformio.ini          # one [env:<logger>] per logger type          (exists)
  lib/
    logger_core/          # feature host, DLOG writer, sinks, config, time, LED, net    (phase 2)
    sensors/              # sensor features: sim accel, ADXL355, BME280, …              (phase 2-3)
  loggers/
    bench_adxl345/        # breadboard ADXL345 streaming for the accel viewer (temporary; exists)
    vibration/main.cpp    # picks features + settings; ~100 lines, no plumbing          (phase 2)
protocol/
  FILE_FORMAT.md          # DLOG data format spec, v1.1 (sessions)       (exists)
  MQTT.md                 # topics + JSON payload schemas, v1            (exists)
  features/               # feature definitions (core, recording, accel, …)   (exists)
master/
  logger_reader/          # Python package: read DLOG -> pandas, sessions, capture tool  (exists)
  logger_master/          # fleet service (MQTT), dashboard, simulator   (phase 5, exists)
registry/
  devices.yaml            # device registry: serials, assignments, expected features (exists)
  models/                 # model catalogue: one page per model (VL-P1.md): specs, LEDs, instructions (exists)
  views/                  # per-logger orientation view settings and 3D models (dashboard; exists)
deploy/
  ubuntu/                 # master PC setup: hotspot, Mosquitto, mDNS, chrony, service (exists)
tools/
  accel-viewer/           # browser bench viewer for the ADXL345 (temporary; exists)
docs/
factory-backup/           # original CircuitPython files from the TinyS3[D]
```

Firmware builds per logger: `cd firmware && pio run -e <logger> -t upload`.

The repo is currently called `Vibration-Logger`. Rename it (e.g. `logger-ecosystem`) once the
second logger type exists.

## Feature model: different devices, common interfaces

Loggers will differ (sensors, storage, power), but anything they share must be controlled the
same way. So every logger is built from **features**: self-contained capabilities, each with a
fixed interface of config fields, status fields, commands, streams and events. Definitions:
[protocol/features/](../protocol/features/).

- **A logger is just a set of features.** The portable vibration logger is `core + recording +
  sd + battery + wifi + accel + publish` (+ `env` if a BME280 is fitted). A future fixed logger
  might be `core + recording + ethernet + netstream + accel×2 + tacho`. The accelerometer
  self-test command is identical on both, because it's the same feature.
- **Instances** let a feature appear more than once (`accel_de`, `accel_nde`). Config and status
  are grouped by instance; commands can name a `target` instance (optional when unambiguous).
- **The feature is the interface, not the hardware.** ADXL355, ADXL345 and the simulator all
  implement `accel`; `info` reports the implementation (`impl`) and narrows allowed values
  (`limits`).
- **Self-describing.** Each logger lists its features in MQTT `info`. The master and dashboards
  read the feature definitions to know what to expect and which controls to show, so a new
  feature needs no per-device-type UI code.
- **Versioned.** Breaking changes to a feature bump its version; adding things doesn't.

Every logger describes its data the same way too (the "common dataframe"), in every DLOG file
header and every `info` message:

- **Device**: `device_id` (MAC, permanent), `serial` (case label), `name`, type, firmware
- **Stream**: channels sampled together on one clock, e.g. `accel` (x, y, z at 4000 Hz) or `env`
  (temperature, humidity at 1 Hz), linked to the feature instance that produced it
- **Channel**: `name`, `unit`, `scale` + `offset` (raw → engineering units); dtype and rate per stream

In firmware, the core library is a **feature host**:

```cpp
class Feature {
public:
    virtual const char* feature() const = 0;            // "accel"
    virtual const char* instance() const = 0;           // "accel" or "accel_de"
    virtual int version() const = 0;
    virtual bool begin(JsonObjectConst config) = 0;     // config section for this instance
    virtual void describe(JsonObject info) const = 0;   // impl, limits, extra info
    virtual void status(JsonObject out) const = 0;
    virtual CommandResult command(const char* cmd, JsonObjectConst args, JsonObject result);
    virtual size_t streams(StreamInfo* out, size_t max) const { return 0; }  // sensors only
    virtual void poll() {}                              // called from the feature's task
};
```

The core routes config sections, commands and status requests to features by instance name,
and sensor features feed sample blocks to the acquisition pipeline. Adding a sensor means
adding a feature class; nothing else changes.

## Device identity and registry

Three identifiers, from the design down to the chip:

| | Example | Identifies | Set by |
|---|---|---|---|
| **Model number** + hardware revision | `VL-P1` rev `A` | The product design: portable vibration logger (TinyS3[D], ADXL355, SD) | The firmware for that model (`device.model`, `device.hardware_rev`) |
| **Serial number** | `VL-0001` | One physical unit | Once when it's built, over USB |
| **`device_id`** | `9070692f3338` | The chip (ESP32 MAC) | Automatic, permanent |

- **Model number:** `VL-P1` = Vibration Logger, Portable, model 1. Each model has a page in the
  **model catalogue**, `registry/models/<model>.md`: specs, the LED guide, operating
  instructions, set-up of a new unit, pin map and troubleshooting, plus the features every unit
  of that model should have. The master dashboard shows it on each logger's About tab.
- **Serial number** (`VL-0001` scheme: model family prefix + 4-digit number): assigned once when
  a unit is built, stored in the board's NVS (survives firmware updates and SD card swaps),
  printed on a label on the case with the model number. Set over USB with a one-off
  `serial VL-0001` command. Included in every DLOG file (`device.serial`) and in `info`.
- **`device_id`**: the ESP32 MAC. Permanent and unique, automatic.
- **The device is the authority on what it can do** (its `info` lists its features). The
  **registry** records what the device can't know about itself:

| Field | Example | Source |
|---|---|---|
| `device_id`, serial | `9070692f3338`, `VL-0001` | Device (auto-added on first contact) |
| Type, model, hardware revision | `vibration`, `VL-P1`, rev A | Device (`info`), or at build |
| Expected features/options | `env` fitted, external antenna | The model's catalogue page; set per unit only for exceptions |
| Sensor details | ADXL355 board serial, calibration offsets | Manual / calibration |
| Assignment | "Pump house 2, motor DE", since date | Manual, changes over time |
| Last seen | firmware, IP, battery, last contact | Automatic from `status` / `info` |
| Notes | "Replaced SD card 2026-11" | Manual |

- The master **compares expected vs advertised** and flags differences (e.g. "VL-0003 should
  have `env` but doesn't"). Expected features come from the unit's own registry entry if set
  there, otherwise from its model's catalogue page; a logger of an unknown model falls back to
  what it reported on first contact.
- **Storage:** a YAML file on the master (`registry/devices.yaml`, next to per-logger
  orientation views and uploaded models in `registry/views/`): easy to edit by hand, fine for a
  handful of units, and it can move into a database later if the fleet grows. It's site data,
  so it's **git-ignored**, not committed: back it up on the master instead. The model catalogue
  (`registry/models/`) is documentation and is in the repo.

## Network and connecting loggers

- **The master's own network:** for now the **Ubuntu PC (master)** runs a 2.4 GHz WPA2 hotspot
  itself (NetworkManager shared mode: the PC is 10.42.0.1 and hands out addresses), set up by
  `deploy/ubuntu/hotspot.sh`. Later a dedicated router can take over; only the loggers'
  `wifi.networks` changes. Either way it's not connected to corporate WiFi: that avoids
  enterprise logins, guest-network client isolation and blocked mDNS, and keeps unmanaged
  devices off IT's network. Ethernet loggers plug into the same network.
- **Ethernet loggers need no setup:** plug in, get an address by DHCP, carry on below.
- **One set of WiFi credentials for the whole fleet**, so connecting a new WiFi logger means
  giving it the same network details (`wifi.networks`, a list tried in order: master network
  first, then e.g. a bench network) as the others. Stored in internal flash, set by any of:
  1. over USB when the unit is built (with the serial number): works for every logger, or
  2. the logger's own hotspot + web page when it can't find a known network (field changes), or
  3. `config.json` on an SD card, for loggers that have one.
- **Everything after that is automatic:** DHCP address → find the broker by mDNS → connect →
  publish `info` → appears on the dashboard and is auto-added to the registry.
- **Limits to know:** ESP32 is 2.4 GHz only; metal/concrete around machinery cuts range (use the
  external antenna, place the AP sensibly); WiFi passwords are stored unencrypted in internal
  flash, and in `config.json` if an SD card is used to set them (acceptable on an isolated
  network; flash encryption can be added later if needed).

## Data format (DLOG)

Full spec: [protocol/FILE_FORMAT.md](../protocol/FILE_FORMAT.md). In short:

- Binary, append-only records (METADATA, DATA, MARKER, END), each with its own CRC32, so a power
  cut loses at most the last partial record and corruption can be skipped.
- Files store **raw sensor counts and axes**. Scaling, axis mapping (mounting) and time
  corrections are metadata applied by the reader.
- Samples are stamped with the device's **monotonic clock**; `time_sync` markers map it to UTC.
  That's how data recorded before a sync gets correct timestamps afterwards.
- Files roll every 60 min / 256 MB and on any change to the stream definitions; one file never
  spans a reboot.
- The same bytes go over TCP for streaming loggers, so streamed and SD recordings are identical.

The Python reader returns one DataFrame per stream with a proper `DatetimeIndex` and units in
the metadata; it can also export CSV/Parquet.

## Data sinks

```cpp
class Sink {
public:
    virtual bool begin(const DeviceInfo&, const std::vector<StreamInfo>&) = 0; // writes header
    virtual bool write(const DataBlock&) = 0;   // must not block acquisition
    virtual void flush() = 0;
    virtual SinkStatus status() const = 0;      // reported in MQTT status
};
```

**Not every logger has an SD card.** Ethernet/streaming loggers may have only a network sink,
so nothing in the core may assume a card: config, identity and boot counters live in the
board's internal flash, and a logger's sinks are just the ones its firmware includes.

- `UsbSink`: DLOG over the USB serial port, for bench capture (phase 2, exists).
- `SdSink`: append to rolling files on SD (phase 3), on loggers that have a card.
- `NetSink`: TCP stream of blocks to the master. If the link drops: fall back to SD if the
  logger has one; otherwise buffer in PSRAM (8 MB ≈ 3-4 min at 36 kB/s) and send on reconnect,
  then drop the oldest data with GAP flagged if the outage lasts longer. Built later; the
  interface exists from day one.
- `PublishSink`: feeds the configured data publications (see MQTT below). Each publication is a
  small `Reducer` (latest, mean, stats, snapshot, …) that consumes blocks and emits one message
  per interval. New methods are new `Reducer` classes registered by name; nothing else changes.
  Runs alongside the recording sinks from the same blocks, so publishing never touches what's
  recorded.

Raw waveforms go over a dedicated TCP stream rather than MQTT. MQTT stays for small control and
status messages, so a busy data stream can't delay commands.

## MQTT protocol

Full spec: [protocol/MQTT.md](../protocol/MQTT.md). Small JSON messages only; raw waveforms go
through a network sink, never MQTT. Each logger publishes retained `info` (identity, features,
streams), `status` (per feature instance; last-will `offline`) and `config` (per feature
instance), plus `events`. Commands are defined by features (`start`, `stop`, `mark`, `identify`,
`set_config`, `self_test`, …) with an optional `target` instance, and can be sent to one logger,
a group (`loggers/group/<group>/cmd`) or everyone; every logger acks individually. Fleet
operations ("self-test every accelerometer in the pump house") are resolved by the master.

**Data publications:** any number of configurable live feeds on `loggers/<id>/data/<name>`.
Each picks a stream, a method (`latest`, `mean`, `stats`, `snapshot`; more can be added by
name), an interval and a window. Messages are self-describing, so new methods need no protocol
change. Best-effort and budget-limited; the recording sinks (SD and/or network stream) remain
the record. Loggers find the broker from
config, with mDNS as a fallback, so no IP addresses are hard-coded.

## Timekeeping

Accuracy target: roughly a second is fine. The master is the time source.

- **Master sets the time.** Loggers sync from the master (NTP) on connect and periodically
  while connected. The portable logger is normally connected, so it stays in sync most of the time.
- **Between syncs** the ESP32 keeps its own time (drift ~1-2 s/day, acceptable).
- **If the clock is lost** (flat battery, reboot in the field) the logger keeps recording,
  timestamps are flagged as *unsynced* (time since boot), and the next sync writes a marker so
  the Python reader can fix those timestamps afterwards. No data is lost.
- Every sync writes a marker into the data file, so drift can be corrected if anyone cares.
- **No RTC on the portable logger.** An RTC (e.g. DS3231) stays possible as an optional add-on
  for future loggers; the core would use one if present.

## Device behaviour (core library)

- **Config** lives in the board's **internal flash** (LittleFS), so every logger has it,
  with or without an SD card. Layers, lowest first:
  1. firmware defaults for the logger type (its feature list and settings);
  2. the stored config in internal flash, changed by `set_config`, the web pages or USB;
  3. on loggers with a card, `config.json` on the SD card, applied at boot and saved to internal
     flash. `set_config` also writes it back to the card, so the two never disagree.
  Secrets never go in the repo.
- **Identity** (serial number, boot counter) lives in NVS, never on removable storage.
- **Acquisition**: sensor reads on core 1 (timer/FIFO driven), buffered in PSRAM; sink writes
  (SD, network) in large blocks from a separate task; networking on core 0. Network or card
  hiccups can't cause dropped samples.
- **Power profiles**: `always_on` (mains), or `duty_cycle` (battery): record window → deep
  sleep → wake, briefly connect, report status, pick up commands.
- **Status LEDs**: the same patterns on every logger (see below).

### Recording control (no button)

Loggers have a **power switch and status LEDs, but no button**. Recording is controlled by
power-on behaviour, config and the network:

- **No auto-start by default** (decided 2026-10-29): after power-on the logger is idle until
  someone starts it, from the dashboard or from the logger's own hotspot page (phone, in the
  field). Stray recordings were more trouble than a missed start. Config `recording.autostart`
  (VL-P1 default `false`) turns auto-start back on, with `recording.start_delay_s` (default 10).
- **Deliberate starts survive restarts:** a recording started by command (dashboard, page, USB)
  is remembered in internal flash; after any restart (brownout, crash, firmware update, reboot)
  it carries on after the start delay, in a new file in the same session, until a `stop`.
- **Connected to the master: no auto-start.** If the logger is connected to the master, the
  master decides when to record, so the countdown is cancelled (event `autostart_skipped`,
  status `recording.autostart: "skipped_master"`) and recording waits for a `start` command.
  If WiFi has joined a network but MQTT isn't connected yet when the countdown ends, it waits up
  to 30 s more (`waiting_for_master`) before starting anyway. `recording.autostart_with_master`
  (default `false`) restores auto-start even with the master connected. (Applies only when
  `autostart` is on.)
- **Battery saver:** `wifi` command `off_until_restart` (a button on the logger's page) turns
  WiFi off completely until the next power-on, for long unattended recordings. While recording
  it also happens by itself `wifi.auto_off_after_s` (default 60) after the last phone leaves
  the logger's hotspot, so a field start is: join hotspot, Start, walk away.
- **Remote control:** `start`, `stop` and `set_config` over WiFi, through either web interface
  (below), or over USB. On loggers with an SD card, config can also be edited in `config.json`
  on the card, no WiFi needed.
- **Switching off while recording** loses at most the last flush interval (default 5 s).
  Earlier data and files are safe. Stop over WiFi first for a perfectly clean END.
- **Faults clear themselves:** if a sink is unavailable (SD card missing, network down) or a
  sensor is missing, the logger shows the error pattern and keeps retrying, so inserting a card
  or restoring the link starts recording without a button.

### Web interfaces

Both send the same commands, so there's one control model:

1. **Master dashboard** (phase 5): a web page on the master PC, talking to every logger over MQTT.
2. **Logger-hosted page** (phase 5): the logger runs its own WiFi hotspot (`VL-xxxxxx`,
   http://192.168.4.1) whenever it isn't connected to the master (`wifi.hotspot: "no_master"`,
   the default, after a 20 s grace). The page shows status, sets the WiFi networks to join and,
   **only while the master isn't connected**, has recording controls: name the session and
   start, or pause (stop). `POST /recording` with `action=start|stop` and optional
   `session=<name>` (start with a session names it and starts, or moves an ongoing recording
   into it; stop pauses until the next start or power-on). With the master connected the
   controls are hidden (`wifi.master` in status), so there's one place in control at a time.
   For the portable logger in the field, from a phone, with no master present.

### Status LEDs

Two **single-colour LEDs mounted in the case wall**, each on a PWM pin through a resistor (zero
idle current, reliable at 3.3 V; ~330 Ω-1 kΩ, 1-3 mA is plenty for an indicator). Each LED has
one job and is read by its pattern. Short pulses, not steady light, while on battery. The
onboard RGB LED mirrors them on its green and blue channels for bench use. `core.led_brightness`
dims both.

**Green: logger status**

| Pattern | Meaning |
|---|---|
| Blinking, faster near the end | Countdown before recording |
| Short pulse every 3 s | Recording |
| Double pulse every 3 s | Recording and connected to the master |
| Slow breathing | Idle: stopped, or auto-start off |
| 1 Hz short blink | Recording with a warning: clock not synced, clipping |
| Fast blink | Can't record: no SD card or sensor fault (retrying) |
| Rapid flashing (both LEDs) | `identify` command |

**Blue: power** (set by the `battery` feature; thresholds are its config, with 3 % hysteresis)

| Pattern | Meaning |
|---|---|
| Off | On battery, above 70 % (`medium_pct`), or no fuel gauge |
| Single pulse every 4 s | On battery, 30-70 % |
| Double pulse every 4 s | On battery, 10-30 % (`low_warning_pct`, `battery_low` event) |
| Burst of 5 fast flashes every 2 s | On battery, below 10 % (`critical_pct`, `battery_critical` event) |
| Slow breathing | On USB, charging |
| Steady | On USB, full: at least 95 % and charge rate ~0 for a minute (the charger's "done" signal isn't wired to the ESP32, so it's inferred from the fuel gauge) |

The same information is in the `battery` status (`percent`, `level`, `charging`, `full`,
`usb_power`), so the master dashboard shows it once WiFi and MQTT arrive in phase 5.

## Hardware: portable vibration logger

| Part | Role | Interface | Notes |
|---|---|---|---|
| Unexpected Maker TinyS3[D] (ESP32-S3, 8 MB PSRAM) | Controller | n/a | Onboard LiPo charger, MAX17048 fuel gauge (I2C), u.FL antenna switch (GPIO38) |
| Analog Devices EVAL-ADXL355-PMDZ | Vibration sensor | SPI + DRDY/INT pin | 20-bit, ±2/4/8 g, very low noise (~25 µg/√Hz), up to 4 kHz output rate (~1 kHz usable bandwidth), 96-entry FIFO, on-chip temperature |
| Adafruit 4682 microSD breakout | Storage | SDIO (SDMMC) 4-bit, or SPI | ESP32-S3 has an SDMMC host; SDIO is faster and uses less CPU than SPI |
| Adafruit 2652 BME280 | Enclosure temperature / humidity (optional) | I2C (shared with fuel gauge) | On a header socket; firmware logs a ~1 Hz `env` stream if it's fitted. Main value is humidity (condensation risk); the ADXL355's on-chip temperature covers heat warnings without it |
| 3.7 V 3000 mAh LiPo (104060) | Power | TinyS3 battery input | **Check connector polarity before plugging in**; generic packs are often reversed |
| 2.4 GHz 3 dBi antenna, SMA to IPEX | Range to master AP | u.FL on TinyS3[D] | Set GPIO38 HIGH to use it; configurable |
| 2x 25 mm pot magnets, M5 thread | Mounting to steel structures | n/a | Fine for the ADXL355's ~1 kHz bandwidth if seated flat on clean steel |
| Perfboard, female headers, brass + nylon standoffs | Assembly | n/a | Sockets let boards be swapped; brass for the sensor path, nylon elsewhere |
| *Bench/spares:* ADXL345 (GY-291), generic microSD adapter | n/a | n/a | ADXL345 used for breadboard bring-up (`bench_adxl345`); noisier but ±16 g, a possible high-g variant later |

**Data rate is small:** ADXL355 at 4 kHz × 3 axes × 20-bit ≈ 36 kB/s. Either SD mode
handles that easily. SDIO is chosen for headroom and lower CPU load.

**Pin map** (in `firmware/loggers/vibration/main.cpp`). 12 of the 14 usable GPIOs (17 exposed,
minus I2C on 8/9 and the IO0 boot pin), leaving IO43 and IO44 spare. If more pins are needed,
run SD in 1-bit SDIO mode (`sd.bus_width: 1`, frees D1-D3; still far faster than the 36 kB/s
needed).

The ADXL355 and LEDs are on the BAT-side header and the SD card on the RX/TX side, so each
board's wires run straight across from one edge. Any GPIO works for SPI/SDMMC/PWM on the
ESP32-S3, so this is purely a layout choice.

| TinyS3[D] pin | Header side | Connects to | Notes |
|---|---|---|---|
| IO4 | BAT side | ADXL355 CS (PMOD pin 1) | own SPI bus (HSPI) |
| IO21 | BAT side | ADXL355 MOSI (pin 2) | |
| IO5 | BAT side | ADXL355 MISO (pin 3) | |
| IO3 | BAT side | ADXL355 SCLK (pin 4) | |
| IO1 | BAT side | case LED, blue (power) | PWM; resistor to the LED anode, cathode to GND |
| IO2 | BAT side | case LED, green (status) | PWM; as above |
| IO34 | RX/TX side | SD CLK | SDMMC host, 1-bit at 20 MHz (bench: ~1.1 MB/s write, logger needs ~40 kB/s) |
| IO6 | RX/TX side | SD CMD (SI) | |
| IO37 | RX/TX side | SD D0 (SO) | |
| IO7, IO35, IO36 | RX/TX side | free | SD DAT1, DAT2 and CS are left unwired in 1-bit mode (the breakout holds CS high) |
| IO8 / IO9 | RX/TX side | I2C SDA / SCL | onboard fuel gauge; BME280 when fitted |
| IO43 / IO44 | RX/TX side (TX / RX) | spare | UART0: the ROM boot log toggles TX at reset |
| IO0 | BAT side | free | BOOT strap pin: anything holding it low at reset enters download mode |
| 3V3 / GND | BAT side / RX/TX side | ADXL355 VDD (pins 6, 12) and DGND (5, 11); SD 3V and GND | 3.3 V only for both |

ADXL355 DRDY (pin 10) isn't wired: the firmware polls the FIFO instead.
The sensor and SD card are on **separate buses**, so slow SD writes can never delay sensor reads.

**Power switch:** between the LiPo and the TinyS3[D]'s BAT input. Off means zero drain (safe
for shelf storage); the battery only charges with the switch on, which also runs the logger
(harmless: it just records while charging).

**Application: machinery vibration.** Defaults: ADXL355 at ±8 g, 4000 Hz output rate (~1 kHz
bandwidth), both configurable.

- **Covers:** overall vibration severity in the ISO 10816/20816 band (10-1000 Hz), plus running speed
  and its harmonics: imbalance, misalignment, looseness, resonance, blade/vane pass on most
  machines.
- **Does not cover:** early bearing-fault and gear-mesh detection (high-frequency/envelope
  analysis, 2-20 kHz). That needs a future logger variant with a wideband sensor (e.g. ADXL1002/1005
  with a fast ADC, or an IEPE accelerometer). The ADXL345 spare doesn't help here (~1.6 kHz max).
- **Clipping:** shocks/impacts can exceed 8 g. The firmware will flag samples at full scale so
  clipped data is obvious in analysis.
- **Analysis:** the Python reader will include the standard machinery metrics (acceleration
  RMS/peak, velocity RMS in mm/s, spectrum/FFT), so raw data turns into numbers comparable with
  ISO severity charts.
- **Environment:** hot machinery is a risk for the LiPo (typically 0-45 °C to charge, ~60 °C max
  to discharge). Mount the battery away from hot surfaces, and use the ADXL355 (or BME280)
  temperature to warn if the enclosure gets too hot. Keep the sensor wiring short near VFDs/motors
  (electrical noise).
- **Not needed:** gyroscope/magnetometer (orientation doesn't matter for vibration analysis).
  Worthwhile future additions are separate sensor modules instead: more measurement points, a
  once-per-revolution **tachometer** (order analysis and phase), bearing surface temperature.

**Bench lessons from the ADXL345 breadboard:**

- Loose breadboard jumpers corrupt SPI reads (impossible values, wrong register reads), worse
  when tapped. The firmware must range-check samples and periodically verify sensor config, and
  the real build uses soldered joints and short wires.
- arduino-esp32 2.0.17 `HWCDC`: `Serial.setTxTimeoutMs(0)` hangs `write()` forever once the host
  closes the port. Use a small non-zero timeout.
- After `pio run -t upload` the TinyS3[D] sometimes stays in download mode; press RST.

**Mechanical:** magnets → rigid base plate → ADXL355 board bolted down with short brass standoffs.
Everything else can sit on nylon standoffs. The sensor path must be stiff; the rest doesn't matter.

**Battery estimate (rough, to be measured with the fuel gauge):** ~40-80 mA average with WiFi
connected → roughly 1.5-3 days on 3000 mAh. Periodic WiFi instead of always-on would extend this.

## Phases

| # | Phase                                                                                                     | Proves                                         |
| - | --------------------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| 0 | Toolchain, TinyS3[D] flashing, WiFi scan, ADXL345 breadboard + accel viewer (done)                        | Hardware works                                 |
| 1 | Restructure repo; write `FILE_FORMAT.md` + `MQTT.md` v1 (done)                                            | Agreed contracts before code                   |
| 1b | Feature model: feature definitions, MQTT/DLOG updates for features, serials, groups (done)               | Common interfaces across different devices     |
| 2 | Core: feature host, DLOG writer, sinks (USB now, SD when wired), config, timekeeping, auto-start + status LED, **simulated accel**; Python reader + capture tool + tests; acquisition/writer tasks with a PSRAM buffer (**built**: compiles, Python tests pass; on-board test outstanding) | Full data path end-to-end without real sensors |
| 3 | ADXL355 feature + SD sink on the Adafruit 4682 (**tested on hardware**: 4000 Hz, no gaps; SD 1-bit 20 MHz ~1 MB/s); config store in internal flash + `set_config` (**built**; SD `config.json` still to do); named sessions (**built**); serial number + WiFi provisioning over USB; registry file | The portable logger is usable |
| 4 | Battery: power manager, fuel gauge (MAX17048 onboard), sleep/duty-cycle                                   | Portable logger is field-ready                 |
| 5 | Master: Ubuntu PC hotspot, Mosquitto, mDNS, chrony (`deploy/ubuntu`); fleet service + dashboard (fleet view, control, sessions, config editor, registry, live plots, orientation) + simulator (**built**: `master/logger_master`, tested against the simulator); WiFi + MQTT status/commands + publications; NTP; logger-hosted web page | Remote control and monitoring |
| 6 | `NetSink` + master stream receiver; Ethernet board support; OTA firmware updates over WiFi (`ota` feature; master side **built**: firmware store, Firmware page, fan-out, simulator) | Streaming loggers                              |
| 7 | Second logger type                                                                                        | The abstraction actually works                 |

Phases 2-4 deliver the portable vibration logger. Phases 5-6 can wait, but nothing in 2-4
blocks them, because the sink interface and file format are designed for streaming from the start.

## Open questions

1. ~~What is being measured~~: decided: machinery. ±8 g, 4000 Hz by default (see Hardware).
2. ~~SD card interface~~: decided: Adafruit 4682 on SDMMC, 1-bit at 20 MHz (4-bit supported; 10 MHz measured ~45 kB/s on this core, so use 20 MHz).
3. Battery life target for the portable logger? Keeping WiFi connected most of the time costs
   power (roughly 20-100 mA), so this decides whether WiFi stays on or connects periodically.
4. ~~Master AP~~: decided: the Ubuntu PC (master) runs the hotspot for now; a dedicated router later (see Network).
5. ~~OTA updates~~: decided: yes, driven from the master over WiFi (`ota` feature, MQTT.md
   "Firmware updates"). The master stores images per model and version and sends `update`;
   each logger downloads its image over HTTP from the master, checks size and SHA-256, writes
   its spare app slot and reboots; new firmware must become healthy within
   `ota.confirm_timeout_s` (default 300 s) or the logger rolls back. Refused while recording.
   The logger's own page also takes an image, for the field; USB flashing stays the recovery
   path.
6. ~~Start on power-up or wait for a command~~: decided (revised 2026-10-29): wait for a
   command (dashboard or the logger's hotspot page); deliberate starts survive restarts;
   auto-start stays available as config (see Recording control).
7. Which SD board is in hand for phase 2: the Adafruit 4682 (SDIO), or the generic adapter (SPI)
   for now? (Phase 2 proceeds with a USB sink until then.)
8. ~~Registry location~~: decided: `registry/devices.yaml` on the master, git-ignored (site
   data stays out of the repo; the model catalogue `registry/models/` is in it).
9. ~~Serial number scheme~~: decided: `VL-0001` (model family prefix + 4-digit number), with the
   model number `VL-P1` rev `A` (see Device identity).
