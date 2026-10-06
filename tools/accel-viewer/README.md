# Accel Viewer (temporary bench tool)

Live 3D orientation, vibration plot and spectrum for the breadboard ADXL345 test firmware.

## Use

1. Flash the test firmware: `cd firmware`, then `pio run -e bench_adxl345 -t upload`
   (source: [firmware/loggers/bench_adxl345/](../../firmware/loggers/bench_adxl345/main.cpp)).
   Press RST on the board if it doesn't start streaming.
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
#ADXL345 rate=800 g_per_lsb=0.0039     info line (rate and scale)
D,<x>,<y>,<z>                           one sample, raw counts
```

## Remove

Delete the `tools/accel-viewer/` folder. Nothing else depends on it.
