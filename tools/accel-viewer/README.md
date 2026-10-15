# Accel Viewer (temporary bench tool)

Live 3D orientation, vibration plot and spectrum for the bench test firmware.

## Use

1. Flash a bench firmware from `firmware/`, then press RST on the board if it doesn't start streaming:
   - ADXL355: `pio run -e bench_adxl355 -t upload`
     ([source](../../firmware/loggers/bench_adxl355/main.cpp): the real ADXL355 driver, 4000 Hz)
   - ADXL345: `pio run -e bench_adxl345 -t upload`
     ([source](../../firmware/loggers/bench_adxl345/main.cpp): 800 Hz)
2. Close any `pio device monitor` (only one program can hold the serial port).
3. Open `index.html` in **Chrome or Edge** (Web Serial isn't supported in Firefox/Safari).
4. Click **Connect** and pick the TinyS3 port.
5. Place the board flat and click **Zero**. This sets "flat" and works out which sensor axis
   points down. It's remembered in the browser.

Optional: **Load model…** shows your own STL, OBJ or GLB (e.g. exported from Onshape) instead
of the default board. Use the Rotate buttons until the coloured X/Y/Z arrows match the real
sensor's axes. The model and its alignment are remembered in the browser.

If the page says Web Serial is unavailable when opened as a file, serve it locally instead:
`python -m http.server -d tools/accel-viewer 8000` then open http://localhost:8000.

## Serial format it expects

```
#ADXL355 rate=4000 g_per_lsb=0.000015625 raw_max=524287   info line (rate, scale, raw range)
#ERROR <message>                                         shown in the viewer for 5 s
D,<x>,<y>,<z>                                            one sample, raw counts
```

## Remove

Delete the `tools/accel-viewer/` folder. Nothing else depends on it.
