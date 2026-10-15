# Vibration-Logger

Firmware and tools for a family of ESP32 data loggers that share one data format and one
control protocol. The first is a portable, battery-powered machinery vibration logger that
records to an SD card.

See [docs/PLAN.md](docs/PLAN.md) for the architecture, hardware and roadmap.

## Layout

| Folder | Contents |
|---|---|
| [firmware/](firmware/) | One PlatformIO project; one build environment per logger type in `loggers/`, shared code in `lib/` |
| [protocol/](protocol/) | The contracts every logger and tool follows: [data format](protocol/FILE_FORMAT.md), [MQTT control](protocol/MQTT.md) and [feature definitions](protocol/features/) |
| [master/](master/) | Python tools for the master PC: the [DLOG reader](master/README.md) (pandas, CSV, USB capture) |
| [tools/](tools/) | Bench tools, e.g. the [accel viewer](tools/accel-viewer/) (temporary) |
| [docs/](docs/) | Plan and design notes |
| [factory-backup/](factory-backup/) | The TinyS3[D]'s original CircuitPython files |

## Building firmware

Needs [PlatformIO](https://platformio.org/) (the VS Code extension or the `pio` CLI).

```powershell
cd firmware
pio run -e vibration -t upload        # build and flash the vibration logger
pio run -e bench_adxl345 -t upload    # or: the ADXL345 bench firmware for the accel viewer
pio device monitor                    # serial output (open after the upload finishes)
```

| Environment | What it is |
|---|---|
| `vibration` | The portable vibration logger: ADXL355 + SD card, plus a DLOG stream on the USB port for bench capture |
| `vibration_sim` | The same logger with a simulated accelerometer, for testing without the sensor wired |
| `bench_adxl355` | ADXL355 (real driver) streaming text for the [accel viewer](tools/accel-viewer/), no SD card needed (temporary) |
| `bench_adxl345` | Breadboard ADXL345 streaming text for the accel viewer (temporary) |

The vibration firmware's USB port carries binary DLOG data while recording, so use
`python -m logger_reader capture` (see [master/](master/README.md)) rather than the monitor to
record it. It also accepts bench commands typed into a serial terminal: `info`, `status`,
`serial VL-0001` (set the unit's serial number) and MQTT-style commands such as
`{"cmd":"identify"}` or `{"cmd":"stop"}`.

The upload and monitor port is set in [firmware/platformio.ini](firmware/platformio.ini)
(currently `COM22`); change it if Windows assigns the board a different COM number.

**TinyS3[D] tips:**

- If the board doesn't run the new firmware after an upload, press **RST** once.
- If an upload can't connect, hold **BOOT**, tap **RST**, release **BOOT**, then upload again.
- Only one program can use the serial port at a time: close the monitor before uploading or
  using the accel viewer.
