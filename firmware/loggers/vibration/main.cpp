// Portable vibration logger (docs/PLAN.md).
//
// Features: core + recording (built in), sd, battery, accel (ADXL355, or the simulator with
// -DUSE_SIM_ACCEL), wifi, publish. Sinks: SD card (the record) and USB (bench capture:
// python -m logger_reader capture COM22 x.dlog). Talks to the master over MQTT when WiFi joins
// its network (set it once over USB: wifi "<ssid>" <password>). Later: env.

#include <adxl355.h>
#include <battery_max17048.h>
#include <logger.h>
#include <mqtt_link.h>
#include <ota_feature.h>
#include <publish_feature.h>
#include <sd_card.h>
#include <sim_accel.h>
#include <wifi_feature.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.4.2"
#endif

// Identifies this image for over-the-air updates: a logger only installs firmware for its own model
static const char FIRMWARE_ID[] = "VLFW-MODEL=VL-P1;VLFW-VERSION=" FIRMWARE_VERSION ";";

// TinyS3[D] pin map (docs/PLAN.md, Pin map). IO8/IO9 are the I2C bus (fuel gauge, BME280);
// IO0 is the BOOT strap pin and stays free. IO1/IO2 drive the case LEDs; IO43/IO44 are spare.
static const logger::Adxl355::Pins ADXL355_PINS = {
    /*sck*/ 3, /*mosi*/ 21, /*miso*/ 5, /*cs*/ 4, /*drdy*/ -1};
static const logger::SdCard::Pins SD_PINS = {
    /*clk*/ 34, /*cmd*/ 6, /*d0*/ 37, /*d1*/ -1, /*d2*/ -1, /*d3*/ -1};  // 1-bit: D1-D3 unwired
static const int PIN_LED_STATUS = 2;   // green: logger status
static const int PIN_LED_POWER = 1;    // blue: battery and charging

// This logger type's defaults; the config stored in internal flash (set_config, the USB wifi
// command) goes on top. Sections are feature instances; fields follow protocol/features/*.json.
// accel_1s feeds the master dashboard's plots and orientation view. No auto-start: recording is
// started from the logger's page or the dashboard (and carries on through restarts until stopped).
static const char* DEFAULT_CONFIG = R"json({
  "core":      { "led_brightness": 0.2 },
  "recording": { "autostart": false, "start_delay_s": 10, "sinks": ["sd", "usb"] },
  "sd":        { "bus_width": 1, "freq_khz": 20000 },
  "accel":     { "range_g": 8, "odr_hz": 4000, "impl": { "spi_hz": 5000000 } },
  "wifi":      { "hotspot": "no_master" },
  "publish":   { "publications": [
    { "name": "accel_1s", "stream": "accel", "method": "stats",
      "params": { "stats": ["mean", "ac_rms", "peak"] }, "interval_s": 1 } ] }
})json";

logger::Logger app("vibration", "TinyS3[D]", FIRMWARE_VERSION);
#ifdef USE_SIM_ACCEL
logger::SimAccel accel("accel");
#else
logger::Adxl355 accel("accel", ADXL355_PINS);
#endif
logger::SdCard sd(SD_PINS);
logger::BatteryMax17048 battery;  // onboard fuel gauge, I2C on IO8/IO9
logger::WifiFeature wifi;
logger::PublishFeature publish;
logger::MqttLink mqtt(wifi);
logger::OtaFeature ota(FIRMWARE_ID);
logger::UsbSink usb;

void setup() {
    app.setModel("VL-P1", "A");  // registry/models/VL-P1.md
    app.led().setPins(PIN_LED_STATUS, PIN_LED_POWER);
    app.add(sd);
    app.add(battery);
    app.add(accel);
    app.add(wifi);
    app.add(publish);
    app.add(ota);
    wifi.setOta(ota);
    app.addSink(sd);
    app.addSink(usb);
    app.setNet(mqtt);
    app.begin(DEFAULT_CONFIG);
}

void loop() {
    app.loop();
}
