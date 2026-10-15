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
  FILE_FORMAT.md          # DLOG data format spec, v1.0                  (exists)
  MQTT.md                 # topics + JSON payload schemas, v1            (exists)
  features/               # feature definitions (core, recording, accel, …)   (exists)
master/
  logger_reader/          # Python package: read DLOG -> pandas, capture tool  (phase 2)
  fleet/                  # MQTT service, dashboard, CLI                 (phase 5)
registry/
  devices.yaml            # device registry: serials, assignments, expected features (phase 3)
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

- **`device_id`**: the ESP32 MAC. Permanent and unique, automatic.
- **Serial number** (e.g. `VL-0001`): assigned once when a unit is built, stored in the board's
  NVS (survives firmware updates and SD card swaps), printed on a label on the case. Set over
  USB with a one-off serial command. Included in every DLOG file (`device.serial`) and in `info`.
- **The device is the authority on what it can do** (its `info` lists its features). The
  **registry** records what the device can't know about itself:

| Field | Example | Source |
|---|---|---|
| `device_id`, serial | `9070692f3338`, `VL-0001` | Device (auto-added on first contact) |
| Type, hardware revision | `vibration`, rev A | At build |
| Expected features/options | `env` fitted, external antenna | At build |
| Sensor details | ADXL355 board serial, calibration offsets | Manual / calibration |
| Assignment | "Pump house 2, motor DE", since date | Manual, changes over time |
| Last seen | firmware, IP, battery, last contact | Automatic from `status` / `info` |
| Notes | "Replaced SD card 2026-11" | Manual |

- The master **compares expected vs advertised** and flags differences (e.g. "VL-0003 should
  have `env` but doesn't").
- **Storage:** start as a file in the repo (`registry/devices.yaml`): easy to edit by hand,
  version history, fine for a handful of units. The master loads it; it can move into a
  database later if the fleet grows.

## Network and connecting loggers

- **The master's own network:** a dedicated 2.4 GHz router/access point (not the PC's hotspot),
  not connected to corporate WiFi. Avoids enterprise logins, guest-network client isolation and
  blocked mDNS, and keeps unmanaged devices off IT's network. Ethernet loggers plug into the
  same router.
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
- **Status LED**: the same colour codes on every logger (see below).

### Recording control (no button)

Loggers have a **power switch and a status LED, but no button**. Recording is controlled by
power-on behaviour, config and the network:

- **Auto-start:** after power-on, the logger counts down and then starts recording. Config
  `record.autostart` (default `true`) and `record.start_delay_s` (default `10`). The delay lets
  you mount it and step away; a `recording` start marker goes in the file regardless.
- **Every power-on re-applies the config.** Stopping over WiFi only lasts until the next power
  cycle, unless `autostart` has been turned off. A crash or brownout therefore resumes
  recording by itself, in a new file (new `boot.id`).
- **Remote control:** `start`, `stop` and `set_config` over WiFi, through either web interface
  (below), or over USB. On loggers with an SD card, config can also be edited in `config.json`
  on the card, no WiFi needed.
- **Switching off while recording** loses at most the last flush interval (default 5 s).
  Earlier data and files are safe. Stop over WiFi first for a perfectly clean END.
- **Faults clear themselves:** if a sink is unavailable (SD card missing, network down) or a
  sensor is missing, the logger shows the error colour and keeps retrying, so inserting a card
  or restoring the link starts recording without a button.

### Web interfaces

Both send the same commands, so there's one control model:

1. **Master dashboard** (phase 5): a web page on the master PC, talking to every logger over MQTT.
2. **Logger-hosted page** (phase 5): the logger runs its own WiFi hotspot when it can't find the
   master, serving a small page for start/stop, status and settings. For the portable logger
   in the field, from a phone, with no master present.

### Status LED

An external **RGB LED mounted in the case wall** (common-cathode, 3 PWM pins with resistors:
zero idle current and reliable at 3.3 V; a single-pin WS2812-type LED is the fallback if pins
run short, at ~1 mA idle). The onboard RGB LED mirrors it for bench use. Short blinks, not
steady light, to save battery.

| Colour / pattern | Meaning |
|---|---|
| Blue blinking, faster near the end | Countdown before recording |
| Green short blink every few seconds | Recording |
| Cyan short blink | Recording and connected to the master |
| Slow white pulse | Idle: stopped, or auto-start off |
| Yellow blink | Recording with a warning: battery low, clock not synced, clipping |
| Red fast blink | Can't record: no SD card or sensor fault (retrying) |
| Rapid white flashing | `identify` command |

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

**Pin map** (in `firmware/loggers/vibration/main.cpp`). 14 of the 14 usable GPIOs (17 exposed,
minus I2C on 8/9 and the IO0 boot pin): it fits, with no spare. If more pins are needed, run SD
in 1-bit SDIO mode (`sd.bus_width: 1`, frees D1-D3; still far faster than the 36 kB/s needed) or
use a single-pin WS2812-type LED (frees 2).

| TinyS3[D] pin | Connects to | Notes |
|---|---|---|
| IO34 | ADXL355 CS (PMOD pin 1) | own SPI bus (HSPI) |
| IO35 | ADXL355 MOSI (pin 2) | |
| IO37 | ADXL355 MISO (pin 3) | |
| IO36 | ADXL355 SCLK (pin 4) | |
| IO7 | ADXL355 DRDY (pin 10) | wired for future use; the FIFO is polled |
| IO1 | SD CLK | SDMMC host, 4-bit |
| IO2 | SD CMD (SI) | |
| IO3 | SD D0 (SO) | |
| IO4 | SD D1 | |
| IO5 | SD D2 | |
| IO6 | SD D3 (CS) | |
| IO8 / IO9 | I2C SDA / SCL | onboard fuel gauge; BME280 when fitted |
| IO21 / IO43 / IO44 | case RGB LED R / G / B | phase 4 (onboard LED used until then) |
| IO0 | free | BOOT strap pin: anything holding it low at reset enters download mode |
| 3V3 / GND | ADXL355 VDD (pins 6, 12) and DGND (5, 11); SD 3V and GND | 3.3 V only for both |
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
| 3 | ADXL355 feature + SD sink on the Adafruit 4682 (**written, compiles; wiring and on-board test next**); sample-rate stability and SD throughput; config store in internal flash (+ optional SD `config.json`), `set_config`; serial number provisioning; registry file | The portable logger is usable |
| 4 | Battery: power manager, fuel gauge (MAX17048 onboard), sleep/duty-cycle                                   | Portable logger is field-ready                 |
| 5 | Master AP setup; WiFi + MQTT status/commands + publications; NTP; fleet service, master dashboard, logger-hosted web page | Remote control and monitoring |
| 6 | `NetSink` + master stream receiver; Ethernet board support; OTA updates                                 | Streaming loggers                              |
| 7 | Second logger type                                                                                        | The abstraction actually works                 |

Phases 2-4 deliver the portable vibration logger. Phases 5-6 can wait, but nothing in 2-4
blocks them, because the sink interface and file format are designed for streaming from the start.

## Open questions

1. ~~What is being measured~~: decided: machinery. ±8 g, 4000 Hz by default (see Hardware).
2. ~~SD card interface~~: decided: Adafruit 4682 on SDMMC 4-bit.
3. Battery life target for the portable logger? Keeping WiFi connected most of the time costs
   power (roughly 20-100 mA), so this decides whether WiFi stays on or connects periodically.
4. ~~Master AP~~: decided: a dedicated 2.4 GHz router/access point (see Network).
5. Is OTA firmware update over the network wanted, or is USB flashing fine?
6. ~~Start on power-up or wait for a command~~: decided: auto-start after a configurable delay
   (default 10 s), no button; start/stop/config over WiFi (see Recording control).
7. Which SD board is in hand for phase 2: the Adafruit 4682 (SDIO), or the generic adapter (SPI)
   for now? (Phase 2 proceeds with a USB sink until then.)
8. ~~Registry location~~: decided: start as `registry/devices.yaml` in the repo.
9. Serial number scheme: `VL-0001` style (type prefix + number)?
