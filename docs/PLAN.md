# Logger Ecosystem: Architecture Plan

Status: draft, updated 2026-10-06 (phase 1 complete)

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
| Fixed streaming logger (future)                | Network, with SD as backup           | Continuous over Ethernet                                                                                                             | Mains            |
| Remote monitor (future)                        | SD + low-rate summaries over network | Periodic                                                                                                                             | Battery or mains |

All three are the same firmware core with a different config and sensor set.

## Repository layout

One repo, one PlatformIO project, one build environment per logger type:

```
firmware/
  platformio.ini          # one [env:<logger>] per logger type          (exists)
  lib/
    logger_core/          # shared: config, time, SD writer, net, MQTT, status, power   (phase 2)
    sensors/              # sensor drivers wrapped in the common Sensor interface        (phase 2-3)
  loggers/
    bench_adxl345/        # breadboard ADXL345 streaming for the accel viewer (temporary; exists)
    vibration/main.cpp    # picks sensors + settings; ~100 lines, no plumbing           (phase 2)
protocol/
  FILE_FORMAT.md          # DLOG data format spec, v1.0                  (exists)
  MQTT.md                 # topics + JSON payload schemas, v1            (exists)
master/
  logger_reader/          # Python package: read files -> pandas        (phase 2)
  fleet/                  # MQTT service + CLI (status, start/stop, configure)  (phase 5)
tools/
  accel-viewer/           # browser bench viewer for the ADXL345 (temporary; exists)
docs/
factory-backup/           # original CircuitPython files from the TinyS3[D]
```

Firmware builds per logger: `cd firmware && pio run -e <logger> -t upload`.

The repo is currently called `Vibration-Logger`. Rename it (e.g. `logger-ecosystem`) once the
second logger type exists.

## The common data model

Every logger describes itself the same way. This description is the "common dataframe": it
goes in every file header and in every MQTT `info` message.

- **Device**: `device_id` (from MAC, permanent), `name` (human label from config), `logger_type`,
  `firmware_version`
- **Stream**: a group of channels sampled together on one clock, e.g. `accel` (x, y, z at
  1000 Hz) or `env` (temperature, humidity at 1 Hz)
- **Channel**: `name`, `unit`, `dtype` (int16/int32/float32), `scale` + `offset` (raw → engineering
  units), `sample_rate`

Sensor modules only declare their streams and channels and push samples. The core handles the rest.

```cpp
class Sensor {
public:
    virtual bool begin(const Config&) = 0;
    virtual StreamInfo describe() const = 0;   // channels, units, rate, scaling
    virtual size_t read(SampleBlock& out) = 0; // called by core's acquisition task
};
```

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

- `SdSink`: append to rolling files on SD (phase 2).
- `NetSink`: TCP stream of blocks to the master; if the link drops, either buffer, fall back
  to SD, or drop (configurable). Built later; the interface exists from day one.
- Possible later: `SummarySink` (compute RMS/peaks per window and publish over MQTT).

Raw waveforms go over a dedicated TCP stream rather than MQTT. MQTT stays for small control and
status messages, so a busy data stream can't delay commands.

## MQTT protocol

Full spec: [protocol/MQTT.md](../protocol/MQTT.md). Small JSON messages only; raw waveforms go
through a network sink, never MQTT. Each logger publishes retained `info`, `status` (last-will
`offline`) and `config`, plus `events` and optional `live` summaries. The master sends commands
(`start`, `stop`, `mark`, `identify`, `set_config`, `sync_time`, `roll_file`, `reboot`) to one
logger or to `loggers/all/cmd`, and every command gets an ack. Loggers find the broker from
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

- **Config**: `config.json` on the SD card (WiFi credentials, broker, sample rates, name).
  Can be overridden via MQTT `set_config`. Secrets never go in the repo.
- **Acquisition**: sensor reads on core 1 (timer/FIFO driven), buffered in PSRAM; SD writes in
  large blocks from a separate task; networking on core 0. Network hiccups can't cause dropped
  samples.
- **Power profiles**: `always_on` (mains), or `duty_cycle` (battery): record window → deep
  sleep → wake, briefly connect, report status, pick up commands.
- **Status LED**: the same colour codes on every logger (recording, error, SD full, no time sync).

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

**Pin budget** (exact GPIOs to be confirmed against the TinyS3[D] pinout during phase 3):
ADXL355 on its own SPI bus (SCK, MOSI, MISO, CS, DRDY = 5 pins), SD on SDMMC 4-bit (CLK, CMD,
D0-D3 = 6 pins), BME280 on the existing I2C bus (0 extra pins). That's 11 of ~14 free GPIOs.
The sensor and SD card are on **separate buses**, so slow SD writes can never delay sensor reads.

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
| 2 | Core: config, `Sink` interface + `SdSink`, timekeeping, **fake sine-wave sensor**; Python reader | Full data path end-to-end without real sensors |
| 3 | Vibration sensor module on real hardware; check sample-rate stability and SD throughput                   | The portable logger is usable                  |
| 4 | Battery: power manager, fuel gauge (MAX17048 onboard), sleep/duty-cycle                                   | Portable logger is field-ready                 |
| 5 | Master AP setup; WiFi + MQTT status/commands; NTP; fleet service + CLI                                    | Remote control and monitoring                  |
| 6 | `NetSink` + master stream receiver; Ethernet board support; OTA updates                                 | Streaming loggers                              |
| 7 | Second logger type                                                                                        | The abstraction actually works                 |

Phases 2-4 deliver the portable vibration logger. Phases 5-6 can wait, but nothing in 2-4
blocks them, because the sink interface and file format are designed for streaming from the start.

## Open questions

1. ~~What is being measured~~: decided: machinery. ±8 g, 4000 Hz by default (see Hardware).
2. ~~SD card interface~~: decided: Adafruit 4682 on SDMMC 4-bit.
3. Battery life target for the portable logger? Keeping WiFi connected most of the time costs
   power (roughly 20-100 mA), so this decides whether WiFi stays on or connects periodically.
4. Master AP: built-in PC WiFi hotspot, or a dedicated router/access point the PC and Ethernet
   loggers plug into? (A dedicated router is more reliable and handles DHCP for both.)
5. Is OTA firmware update over the network wanted, or is USB flashing fine?
6. Should loggers start recording on power-up, or wait for a command/button?
