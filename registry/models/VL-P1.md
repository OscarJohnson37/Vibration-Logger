---
model: VL-P1
name: Portable vibration logger
type: vibration
hardware_revs: [A]
description: Battery-powered 3-axis vibration logger for machinery. Records to a microSD card; talks to the master over WiFi when it's in range.
expected_features: [core, recording, sd, battery, accel, wifi, publish]
specs:
  controller: Unexpected Maker TinyS3[D] (ESP32-S3, 8 MB PSRAM)
  accelerometer: ADXL355, 3 axes, 20-bit, ±2 / ±4 / ±8 g, 125 to 4000 samples/s (default ±8 g, 4000 Hz)
  storage: microSD (Adafruit 4682), SDMMC 1-bit at 20 MHz, about 130 MB per hour at 4000 Hz
  battery: 3.7 V 3000 mAh LiPo, MAX17048 fuel gauge, charges over USB-C with the power switch on
  indicators: green status LED, blue power LED (case wall)
  network: 2.4 GHz WiFi; MQTT to the master; own hotspot when the master isn't connected
---

# VL-P1 portable vibration logger

A battery-powered logger for machinery vibration. It records three-axis acceleration to a
microSD card and needs no network to do it. When it's in range of the master's WiFi it reports
its status, takes commands and keeps its clock set; away from the master you control it from a
phone through its own hotspot.

| Part | Details |
|---|---|
| Controller | Unexpected Maker TinyS3[D] (ESP32-S3, 8 MB PSRAM) |
| Accelerometer | ADXL355: 3 axes, 20-bit, ±2 / ±4 / ±8 g, 125 to 4000 samples/s. Default ±8 g at 4000 Hz |
| Storage | microSD card (Adafruit 4682 breakout), 1-bit SDMMC at 20 MHz. About 130 MB per hour at 4000 Hz, so a 32 GB card holds roughly 9 days |
| Battery | 3.7 V 3000 mAh LiPo with a MAX17048 fuel gauge. Charges from USB-C while the power switch is on |
| Indicators | Green status LED and blue power LED in the case wall (the board's RGB LED shows the same) |
| Network | 2.4 GHz WiFi only. MQTT to the master; its own hotspot `VL-xxxxxx` when the master isn't connected |
| Antenna | Internal; a u.FL socket for an external antenna is fitted, but selecting it isn't in the firmware yet |
| Controls | Power switch only, no buttons |

## LED guide

LEDs run at 20% brightness by default (`core.led_brightness`).

### Green: status

| Pattern | Meaning |
|---|---|
| Blinking once a second, then fast (4 per second) for the last 3 s | Counting down to start: carrying on after a restart (10 s), or auto-start if it's been turned on |
| One short flash every 3 s | **Recording** |
| Two short flashes every 3 s | Recording and connected to the master |
| Slow breathing (4 s) | Idle: powered and healthy, not recording |
| Short blink once a second | Warning |
| Fast blinking, about 3 per second | **Error**: a part isn't working (sensor not responding, no SD card) or there's nowhere to record. It keeps retrying; fixing the problem (e.g. inserting a card) clears it |

### Blue: power

| Pattern | Meaning |
|---|---|
| Off | On battery, charge high (above 70%), or no battery fitted |
| One flash every 4 s | On battery, medium (30 to 70%) |
| Two flashes every 4 s | On battery, low (10 to 30%): charge soon |
| Five quick flashes every 2 s | On battery, critical (below 10%) |
| Slow breathing (3 s) | On USB, charging |
| Steady | On USB, battery full |

### Both

| Pattern | Meaning |
|---|---|
| Both LEDs flashing rapidly | **Identify**: someone pressed Identify on the dashboard to find this unit |

## Using it

### Powering on

1. Switch it on. It does **not** start recording by itself: green breathes slowly (idle).
2. **Start it:** from the master dashboard, or away from the master from your phone on the
   logger's own hotspot page (below). Green then flashes once every 3 s (twice with the master
   connected).
3. **Restarts carry on:** once started, a recording continues after any restart (a power
   glitch, a firmware update, a reboot) — after a 10 s countdown, in a new file in the same
   session — until someone presses **Stop**. Switching off while recording loses at most the
   last 5 s.

(Auto-start after power-on can be turned back on with `recording.autostart`; it's skipped when
the master is connected.)

### Naming a session, pausing (from a phone, away from the master)

Recordings are grouped into **sessions**, e.g. `pump2-baseline`. Name one before or while
recording so the files aren't filed under `unnamed` or a date.

1. On your phone, join the WiFi network **VL-** plus the last 6 characters of the logger's ID
   (shown on the label, e.g. `VL-2F3338`). It's open; if the phone warns there's no internet,
   stay connected.
2. Open **http://192.168.4.1**.
3. Under **Recording**, type a session name and press **Start**. A logger that's already
   recording moves into the new session (new file). **Pause** stops recording until you press
   Start again or the logger is switched off and on.

The session name is remembered, so after a restart the recording carries on in the same session.

Starting from this page tells the logger you're working without the master: it stops looking
for one until it's switched off and on. To make the battery last, WiFi switches off by itself **a
minute after you disconnect from the hotspot while it's recording** (or press **Turn WiFi off until restart** straight away): the
hotspot and the search for the master switch off and recording carries on. Reconnecting within
the minute cancels it. Switch the logger off and on to get WiFi back.

**Home-screen icon:** with the page open, *Share → Add to Home Screen* (iPhone) or *⋮ → Add to
Home screen* (Android). It works for any logger, since every logger's hotspot page is at
192.168.4.1.
The recording controls only appear while the logger isn't connected to the master.

### With the master

Open the master dashboard (on the master PC's hotspot: **http://10.42.0.1:8080**). Each logger
shows its battery, recording state, session, SD card space and sensor health. From there you
can start (with a session name), stop, identify, mark notes in the data, sync clocks, run
self-tests, change settings and see live vibration levels and orientation.

### Giving it a WiFi network

Either:
- **From its hotspot page:** join `VL-xxxxxx`, open http://192.168.4.1, fill in **Network**
  (name and password; leave the master address blank on the master's own hotspot) and press
  **Save & reboot**. If it can't find the network, its hotspot comes back after about 15 s.
- **Over USB:** in a serial terminal (115200 baud), type `wifi "Network name" password`, then
  power-cycle it. `wifi clear` forgets all networks.

It can remember several networks and tries them in order.

### Clock

The master sets the logger's clock when it connects, and every hour after. Recordings made
before the clock was set are still timed correctly afterwards: the reader corrects them from the
first sync. Without a master the clock is only known relative to power-on until the next sync.

### Battery and charging

- Plug in USB-C **with the power switch on** to charge; the blue LED breathes while charging
  and goes steady when full.
- Switch off for storage: off means no drain at all.
- On USB without a battery the gauge reads about 100%; that's the charger, not a battery.

### Getting the data

Switch off, take out the microSD card and copy it to a PC. Files are under:

```
/logs/<session>/<serial>_b<boot>_f<file>.dlog
/logs/pump2-baseline/VL-0001_b000042_f0003.dlog
```

A new file starts every hour, on each power-on, and when the session changes. Read them in
Python:

```
python -m logger_reader info /path/to/logs/pump2-baseline/*.dlog
python -m logger_reader export /path/to/file.dlog --stream accel --csv out.csv
```

or in code with `logger_reader.read_session(...)` / `find_sessions(...)` (see master/README.md).

## Setting up a new unit

1. Flash the firmware from `firmware/`: `pio run -e vibration -t upload`.
2. Open a serial terminal (115200 baud) and give it its serial number, once:
   `serial VL-0001` (next free number, from `registry/devices.yaml`).
3. Give it the master's WiFi: `wifi "logger-net" <password>`.
4. Power-cycle it. It appears on the dashboard and is added to the registry automatically.
5. Label the case with the model (VL-P1 rev A), serial number and hotspot name (`VL-` + last 6
   characters of the device ID, shown on the dashboard).
6. Check: insert a card, run **SD self-test** and **Accelerometer self-test** from the
   dashboard's Control tab.

## Firmware updates

Firmware 0.4.0 and later updates over WiFi from the master:

1. On the dashboard, open **Firmware** (top right) and upload the new `firmware.bin` (built
   with `pio run -e vibration`; model and version fill in from the file).
2. Stop any logger you want to update: it refuses while recording.
3. Tick the loggers (or **Update all VL-P1**) and confirm.
4. Each logger downloads the image (progress on its card), restarts, and comes back on the new
   version. It's offline for a few seconds while it restarts.
5. It then has 5 minutes (`ota.confirm_timeout_s`) to prove the new firmware works: sensors OK
   and back on the master. If not, it switches back to the previous firmware by itself; the
   dashboard shows **rolled back** in its Events and the card, and it carries on as before.

A failed download changes nothing: the old firmware keeps running. Away from the master, the
logger's own page (hotspot `VL-xxxxxx`, http://192.168.4.1) also takes a `firmware.bin`.

**USB** always works, and is the way back if a logger won't start at all:
`pio run -e vibration -t upload` from `firmware/` (hold BOOT and press RST if it isn't found).
Settings, serial number and WiFi details survive a firmware update.

## Pin map (hardware rev A)

| TinyS3[D] pin | Connects to | Notes |
|---|---|---|
| IO4 | ADXL355 CS (PMOD pin 1) | own SPI bus |
| IO21 | ADXL355 MOSI (pin 2) | |
| IO5 | ADXL355 MISO (pin 3) | |
| IO3 | ADXL355 SCLK (pin 4) | |
| 3V3 / GND | ADXL355 VDD (pin 6 or 12) / GND (pin 5 or 11) | pins 6 & 12 and 5 & 11 are joined on the board |
| IO34 | SD CLK | SDMMC 1-bit |
| IO6 | SD CMD (labelled SI) | |
| IO37 | SD D0 (labelled SO) | |
| 3V3 / GND | SD 3V / GND | 3 V only, never 5 V |
| — | SD CS, DAT1, DAT2, DET | not connected (the breakout holds CS high) |
| IO1 | Blue power LED, through 68 to 100 Ω to the anode; cathode to GND | |
| IO2 | Green status LED, through 150 to 220 Ω to the anode; cathode to GND | |
| IO8 / IO9 | I2C SDA / SCL | onboard fuel gauge (BME280 if fitted later) |
| BAT pads (back) | LiPo through the power switch | **check the battery connector's polarity** |
| IO0, IO7, IO35, IO36, IO43, IO44 | free | IO0 is the boot pin; leave it free |

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| Green fast blinking, SD card "not detected" | Card missing or not seated; SD board unpowered; CMD/D0 wired to other pins | Reseat the card. Run **SD self-test**: its pin check shows each line. CMD and D0 reading low together means the SD board has no power or they're on different pins than the firmware expects |
| SD works but slowly / only in 1-bit | Normal for this model: it's wired for 1-bit | Nothing to do (1-bit at 20 MHz is about 25× faster than needed) |
| Accelerometer "not responding" | Wiring or power to the ADXL355 | The status shows the ID bytes it read (should be `AD 1D ED`). `FF FF FF`: no power or MISO open. `00 00 00`: MISO held low (short, or sensor unpowered) |
| Doesn't appear on the dashboard | Not on the master's network, wrong password, or the master's firewall | Join its hotspot `VL-xxxxxx` (it appears when the master isn't connected) and check the Network section; on a Windows master allow port 1883 through the firewall |
| Not recording after power-on | Normal: loggers don't auto-start | Start it from the dashboard or its hotspot page |
| Battery reads ~100% on USB | No battery connected (or switch off): the gauge is measuring the charger | Switch on with the battery fitted; check the reading on battery alone |
| Clock "not synced" | No master time server reached | On the master, check chrony; or press **Set logger clocks from master** on the dashboard |
