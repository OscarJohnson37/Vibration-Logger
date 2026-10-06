# DLOG data format, v1.0

Status: draft v1.0, 2026-10-06

The single data format for every logger in the ecosystem. The same bytes are used for files on
an SD card and for live network streams, and one Python reader handles both.

## Design goals

- **Self-describing.** A file says which device wrote it, what each channel is, its units and
  scaling. No external lookup is needed to interpret it.
- **Survives power cuts.** Append-only records, each with its own CRC. A cut mid-write loses at
  most the last partial record; corruption in the middle can be skipped.
- **Compact.** Raw integer samples, not text. 4 kHz × 3 axes × 24-bit ≈ 36 kB/s.
- **Multi-stream.** Any number of streams at different rates (e.g. 4000 Hz acceleration
  alongside 1 Hz temperature) in one file.
- **Raw data stays raw.** Files hold the sensor's own counts and axes. Scaling, axis mapping and
  time corrections are metadata applied by the reader, so nothing is lost if they change later.
- **Extensible.** Readers skip record types and JSON fields they don't know.

## Conventions

- All integers are **little-endian**.
- `u8/u16/u32/u64` are unsigned, `i16/i24/i32` signed two's complement.
- JSON is UTF-8, no byte-order mark.
- CRC32 is the standard IEEE 802.3 polynomial, the same as Python's `zlib.crc32` and the
  ESP32 ROM's `esp_rom_crc32_le(0, …)`.
- Times are microseconds (`_us`) unless named otherwise.

## File layout

```
┌──────────────────────┐
│ Preamble (16 bytes)  │
├──────────────────────┤
│ METADATA record      │  exactly one, always first
├──────────────────────┤
│ DATA / MARKER records│  any number, in time order per stream
│ …                    │
├──────────────────────┤
│ END record           │  only if the file was closed cleanly
└──────────────────────┘
```

### Preamble

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | magic | `89 44 4C 47 0D 0A 1A 0A` (`\x89DLG\r\n\x1a\n`) |
| 8 | 2 | version_major | `1` |
| 10 | 2 | version_minor | `0` |
| 12 | 4 | reserved | `0` |

The magic bytes catch files mangled by text-mode transfers, like PNG's. A reader must refuse a
file whose `version_major` it doesn't support. Minor versions only add things, so older
readers can still read newer minor versions.

### Record framing

Every record after the preamble has the same frame:

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | sync | `A5 5A`. Lets a reader re-find record boundaries after corruption |
| 2 | 1 | type | See record types below |
| 3 | 1 | flags | Reserved, write `0` |
| 4 | 4 | length | Payload length in bytes (max 1 MiB) |
| 8 | length | payload | |
| 8 + length | 4 | crc32 | Over bytes 2 … 8 + length − 1 (type, flags, length, payload) |

### Record types

| Type | Name | Payload |
|---:|---|---|
| `0x01` | METADATA | JSON object, see [Metadata](#metadata) |
| `0x02` | DATA | Binary sample block, see [DATA](#data-records) |
| `0x03` | MARKER | Timestamp + JSON object, see [MARKER](#marker-records) |
| `0x7E` | END | Empty. The writer closed the file cleanly |
| `0x80`–`0xFF` | | Reserved for experiments; readers skip them |

Readers skip any other unknown type using `length`.

## DATA records

One block of consecutive frames from one stream. A **frame** is one sample of every channel in
the stream, taken at the same instant.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | stream_id (u16) | Matches a stream `id` in the metadata |
| 2 | 2 | flags (u16) | Bit field, see below |
| 4 | 4 | n_frames (u32) | Frames in this block |
| 8 | 8 | first_frame (u64) | Index of this block's first frame, counted from the start of recording in this boot |
| 16 | 8 | t_mono_us (u64) | Device monotonic time of the first frame (µs since boot) |
| 24 | … | samples | `n_frames × channels × dtype size` bytes, frame-major: `x0 y0 z0 x1 y1 z1 …` |

`length` must equal `24 + n_frames × channels × size`; a reader treats a mismatch as corruption.

**Flags:**

| Bit | Name | Meaning |
|---:|---|---|
| 0 | CLIPPED | At least one sample in this block is at the stream's raw full-scale limit |
| 1 | GAP | Frames were lost immediately before this block (e.g. sensor FIFO overflow). `first_frame` says how many |
| 2 | SUSPECT | Sensor or bus errors occurred while this block was being read; values may be unreliable |
| 3–15 | | Reserved, write `0` |

**Gaps:** frames are contiguous when `first_frame == previous.first_frame + previous.n_frames`.
Anything else is a gap of that many frames, whether or not GAP is set.

**Sample timing:** frame `i` of a block was sampled at `t_mono_us + i × 1e6 / rate_hz`. The
sensor's real rate differs slightly from nominal (oscillator tolerance, typically < 1%). Readers
should estimate the effective rate by fitting `t_mono_us` against `first_frame` across blocks.

**Block size:** writers should aim for roughly 0.1–1 s of data per block (e.g. 1000 frames at
4 kHz). Smaller blocks waste space on headers; larger blocks lose more on a power cut.

### Sample types (`dtype`)

| dtype | Bytes | Notes |
|---|---:|---|
| `i16` | 2 | |
| `i24` | 3 | Packed little-endian, sign-extended on read. For 20-bit sensors such as the ADXL355 |
| `i32` | 4 | |
| `u16` | 2 | |
| `u32` | 4 | |
| `f32` | 4 | IEEE 754. For derived or already-scaled values only |

All channels in one stream share a dtype. Engineering value = `raw × scale + offset`, using the
channel's `scale` and `offset`.

## MARKER records

Things that happen at a point in time, other than samples.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | t_mono_us (u64) |
| 8 | … | JSON object with at least `"type"` |

| type | Fields | Written when |
|---|---|---|
| `time_sync` | `utc_us`, `source` (`"ntp"`, `"manual"`, `"gps"`), `uncertainty_us` | The device clock was set or checked. `utc_us` is the UTC time at `t_mono_us` |
| `recording` | `state` (`"start"`, `"stop"`, `"pause"`, `"resume"`), `reason` | Recording state changes |
| `note` | `text`, `source` (`"user"`, `"master"`) | A note from a button press or the master, e.g. "pump started" |
| `event` | `name`, plus free fields | Notable conditions: `battery_low`, `temp_high`, `sensor_error`, `sd_error`, `clipping` |
| `config` | `changes` (object) | Settings changed that don't affect stream definitions |

Readers keep markers of unknown types and expose them, unchanged.

## Metadata

The METADATA record describes everything needed to interpret the file. A writer **must start a
new file** whenever anything in it would change (streams, rates, ranges, scaling), so it holds
for the whole file. One file never spans a reboot.

```json
{
  "format": "dlog",
  "format_version": "1.0",
  "device": {
    "id": "9070692f3338",
    "name": "vib-01",
    "type": "vibration",
    "hardware": "TinyS3[D]",
    "firmware": "0.2.0"
  },
  "boot": { "id": 42, "reason": "power_on" },
  "file": { "index": 3, "opened_mono_us": 3600123456 },
  "time": {
    "synced": true,
    "utc_us": 1791270000000000,
    "mono_us": 3600123000,
    "source": "ntp",
    "uncertainty_us": 5000
  },
  "streams": [
    {
      "id": 1,
      "name": "accel",
      "sensor": { "model": "ADXL355", "range_g": 8, "odr_hz": 4000, "lpf_hz": 1000, "hpf": "off" },
      "rate_hz": 4000,
      "dtype": "i24",
      "raw_min": -524288,
      "raw_max": 524287,
      "channels": [
        { "name": "x", "unit": "g", "scale": 1.5625e-5, "offset": 0 },
        { "name": "y", "unit": "g", "scale": 1.5625e-5, "offset": 0 },
        { "name": "z", "unit": "g", "scale": 1.5625e-5, "offset": 0 }
      ],
      "mounting": {
        "axis_map": [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
        "axis_names": ["axial", "horizontal", "vertical"],
        "location": "motor DE bearing"
      }
    },
    {
      "id": 2,
      "name": "env",
      "sensor": { "model": "BME280" },
      "rate_hz": 1,
      "dtype": "f32",
      "channels": [
        { "name": "temperature", "unit": "degC", "scale": 1, "offset": 0 },
        { "name": "humidity", "unit": "%RH", "scale": 1, "offset": 0 }
      ]
    }
  ],
  "config": { }
}
```

| Field | Required | Notes |
|---|:-:|---|
| `format`, `format_version` | ✓ | Always `"dlog"`; the version repeats the preamble's |
| `device.id` | ✓ | Permanent unique ID: the ESP32 MAC, 12 lowercase hex digits |
| `device.name` | | Human label from config; may change between files |
| `device.type` | ✓ | Logger type, e.g. `"vibration"` |
| `device.hardware`, `device.firmware` | ✓ | |
| `boot.id` | ✓ | Increments every boot, stored on the device. Monotonic times are only comparable within one boot |
| `boot.reason` | | `"power_on"`, `"brownout"`, `"watchdog"`, `"software"`, … |
| `file.index` | ✓ | File number within this boot, from 0 |
| `time` | ✓ | Clock state when the file opened. If `synced` is false, only `mono_us` is present |
| `streams[].id` | ✓ | Unique within the file; referenced by DATA records |
| `streams[].name` | ✓ | Short machine name: `accel`, `env`, `tacho`, … |
| `streams[].rate_hz` | ✓ | Nominal frame rate |
| `streams[].dtype` | ✓ | See sample types |
| `streams[].raw_min`, `raw_max` | | Sensor full scale in raw counts; used for clipping detection |
| `streams[].channels[]` | ✓ | `name`, `unit`, `scale`, `offset` each required |
| `streams[].sensor` | | Sensor model and settings, free-form |
| `streams[].mounting` | | How the sensor is installed. `axis_map` (3×3, rows = reported axes, columns = sensor axes) is applied by the reader, not to the stored data |
| `config` | | Snapshot of the device configuration, secrets removed |

Readers ignore fields they don't recognise.

## Time model

Every sample is stamped with the device's **monotonic clock** (µs since boot), which never jumps.
Wall-clock time is a separate mapping, `utc = mono + offset`, supplied by the metadata `time`
block and `time_sync` markers.

Readers convert as follows:

1. **Synced:** use the nearest `time_sync` before the sample; between two syncs, interpolate the
   offset linearly to correct for clock drift.
2. **Not yet synced:** samples before the first sync in a boot use the **first later**
   `time_sync` from the same boot (in this file or later files of the same `device.id` and
   `boot.id`). This is how recordings made away from the master get correct times afterwards.
3. **Never synced in this boot:** times are only known relative to boot. The reader returns
   relative times and flags the data as unsynced.

Accuracy target: about 1 s. The ESP32 clock drifts roughly 1–2 s per day between syncs.

## Files on the SD card

```
/logs/<device.id>/b<boot.id, 6 digits>_f<file.index, 4 digits>.dlog
/logs/9070692f3338/b000042_f0003.dlog
```

Names don't depend on wall-clock time, which may be unknown when a file is opened.

Writers must:

- Write the preamble and METADATA, and flush them, when opening a file.
- Flush to the card at least every **5 s** (configurable). This bounds data lost on a power cut.
- Roll to a new file every **60 min** or **256 MB**, whichever comes first (both configurable),
  and whenever the metadata would change. Stays well under FAT32's 4 GB limit.
- Write END and close the file on a clean stop.

Typical sizes for the vibration logger (ADXL355, 3 × i24):

| Rate | Data rate | Per hour | Per day |
|---:|---:|---:|---:|
| 1000 Hz | 9 kB/s | 32 MB | 0.8 GB |
| 4000 Hz | 36 kB/s | 130 MB | 3.1 GB |

## Reading rules

1. Check the magic and `version_major`.
2. Read records in order and check each CRC.
3. **CRC failure or impossible length:** count the error, scan forward for the next `A5 5A`
   whose record passes its CRC, and continue from there.
4. **File ends mid-record:** drop the partial record. That's normal after a power cut.
5. Skip unknown record types; keep unknown marker types and JSON fields.
6. **No END record:** the file wasn't closed cleanly. Report it, but the data is still valid.
7. Report CRC errors, gaps, CLIPPED and SUSPECT blocks with the data, so analysis can account
   for them.

## Network streaming

A logger with a network sink sends **exactly the same byte sequence** over a TCP connection
to the master's stream receiver: preamble, METADATA, records, then END on a clean stop. There
is no extra framing. The receiver writes the bytes to disk using the same naming scheme, so
streamed and SD-card recordings are identical files. A dropped connection is handled like a
power cut: the receiver keeps everything up to the last complete record, and the logger starts
a new file (a new preamble and METADATA) when it reconnects.

## Changes

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-10-06 | First version |
