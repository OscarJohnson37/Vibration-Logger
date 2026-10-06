# Vibration-Logger

Firmware and tools for a family of ESP32 data loggers that share one data format and one
control protocol. The first is a portable, battery-powered machinery vibration logger that
records to an SD card.

See [docs/PLAN.md](docs/PLAN.md) for the architecture, hardware and roadmap.

## Layout

| Folder | Contents |
|---|---|
| [firmware/](firmware/) | One PlatformIO project; one build environment per logger type in `loggers/` |
| [protocol/](protocol/) | The contracts every logger and tool follows: [data format](protocol/FILE_FORMAT.md) and [MQTT control](protocol/MQTT.md) |
| [tools/](tools/) | Bench tools, e.g. the [accel viewer](tools/accel-viewer/) (temporary) |
| [docs/](docs/) | Plan and design notes |
| [factory-backup/](factory-backup/) | The TinyS3[D]'s original CircuitPython files |

## Building firmware

Needs [PlatformIO](https://platformio.org/) (the VS Code extension or the `pio` CLI).

```powershell
cd firmware
pio run -e bench_adxl345 -t upload    # build and flash
pio device monitor                    # serial output (open after the upload finishes)
```

The upload and monitor port is set in [firmware/platformio.ini](firmware/platformio.ini)
(currently `COM22`); change it if Windows assigns the board a different COM number.

**TinyS3[D] tips:**

- If the board doesn't run the new firmware after an upload, press **RST** once.
- If an upload can't connect, hold **BOOT**, tap **RST**, release **BOOT**, then upload again.
- Only one program can use the serial port at a time: close the monitor before uploading or
  using the accel viewer.
