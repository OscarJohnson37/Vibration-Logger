// Portable vibration logger (docs/PLAN.md).
//
// Features: core + recording (built in), sd, accel (ADXL355, or the simulator with -DUSE_SIM_ACCEL).
// Sinks: SD card (the record) and USB (bench capture: python -m logger_reader capture COM22 x.dlog).
// Later: battery, wifi, publish, env.

#include <adxl355.h>
#include <logger.h>
#include <sd_card.h>
#include <sim_accel.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.3.0-dev"
#endif

// TinyS3[D] pin map (docs/PLAN.md, Pin map). IO8/IO9 are the I2C bus (fuel gauge, BME280);
// IO0 is the BOOT strap pin and stays free. IO21/43/44 are reserved for the case RGB LED.
static const logger::Adxl355::Pins ADXL355_PINS = {
    /*sck*/ 36, /*mosi*/ 35, /*miso*/ 37, /*cs*/ 34, /*drdy*/ 7};
static const logger::SdCard::Pins SD_PINS = {
    /*clk*/ 1, /*cmd*/ 2, /*d0*/ 3, /*d1*/ 4, /*d2*/ 5, /*d3*/ 6};

// This logger type's defaults. Phase 3b layers the stored config (internal flash, and
// config.json on the SD card) on top. Sections are feature instances; fields follow
// protocol/features/*.json.
static const char* DEFAULT_CONFIG = R"json({
  "core":      { "led_brightness": 0.2 },
  "recording": { "autostart": true, "start_delay_s": 10, "sinks": ["sd", "usb"] },
  "sd":        { "bus_width": 4, "freq_khz": 10000 },
  "accel":     { "range_g": 8, "odr_hz": 4000, "impl": { "spi_hz": 5000000 } }
})json";

logger::Logger app("vibration", "TinyS3[D]", FIRMWARE_VERSION);
#ifdef USE_SIM_ACCEL
logger::SimAccel accel("accel");
#else
logger::Adxl355 accel("accel", ADXL355_PINS);
#endif
logger::SdCard sd(SD_PINS);
logger::UsbSink usb;

void setup() {
    app.add(sd);
    app.add(accel);
    app.addSink(sd);
    app.addSink(usb);
    app.begin(DEFAULT_CONFIG);
}

void loop() {
    app.loop();
}
