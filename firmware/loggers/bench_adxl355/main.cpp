// Bench test: ADXL355 streamed as text for tools/accel-viewer (temporary).
//
// Uses the real Adxl355 feature and the Logger's acquisition/writer pipeline, with a text sink
// in place of USB DLOG, so the viewer exercises the same driver the vibration logger records
// with. If an SD card is fitted it also records real DLOG files to it, and the viewer's
// "Test SD" button runs the sd self_test. Works without a card.
// It also runs a WiFi hotspot (VL-xxxxxx) with a status page at http://192.168.4.1, for checking
// the battery with USB unplugged.
//
// Serial protocol (the viewer's):
//   #ADXL355 rate=<Hz> g_per_lsb=<scale> raw_max=<counts>   info line, every 2 s
//   #ERROR <message>                                         sensor problem, every 2 s while it lasts
//   #BATTERY voltage=<V> percent=<%> usb=<0|1> charging=<0|1> battery level, every 2 s
//   #SD present=<0|1> free_mb=<MB> total_mb=<MB> file=<path>  SD card, every 2 s
//   D,<x>,<y>,<z>                                            one sample, raw counts
// The usual bench console commands (status, info, {"cmd":...}) also work; replies start with '#'.

#include <adxl355.h>
#include <battery_max17048.h>
#include <logger.h>
#include <sd_card.h>
#include <wifi_feature.h>

#include <vector>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "bench"
#endif

// Same wiring as the vibration logger (docs/PLAN.md, Pin map)
static const logger::Adxl355::Pins ADXL355_PINS = {
    /*sck*/ 3, /*mosi*/ 21, /*miso*/ 5, /*cs*/ 4, /*drdy*/ -1};
static const logger::SdCard::Pins SD_PINS = {
    /*clk*/ 34, /*cmd*/ 6, /*d0*/ 37, /*d1*/ -1, /*d2*/ -1, /*d3*/ -1};  // 1-bit: D1-D3 unwired
static const int PIN_LED_STATUS = 2;   // green: logger status
static const int PIN_LED_POWER = 1;    // blue: battery and charging

static const char* CONFIG = R"json({
  "recording": { "autostart": true, "start_delay_s": 2, "sinks": ["text", "sd"] },
  "sd":        { "bus_width": 1, "freq_khz": 20000 },
  "wifi":      { "hotspot": "always" },
  "accel":     { "range_g": 8, "odr_hz": 4000, "impl": { "spi_hz": 5000000 } }
})json";

// Prints every frame as a "D,x,y,z" line. Runs in the Logger's writer task.
class TextSink : public logger::Sink {
public:
    const char* name() const override { return "text"; }
    bool open(const String&, const String&, const String&) override { return true; }
    void write(const logger::SampleBlock& b) override {
        if (!b.i32) return;
        const int ch = b.stream->nChannels;
        out_.resize(size_t(b.nFrames) * 28 + 1);
        size_t len = 0;
        for (uint32_t f = 0; f < b.nFrames; f++) {
            const int32_t* v = b.i32 + size_t(f) * ch;
            len += snprintf(out_.data() + len, out_.size() - len, "D,%ld,%ld,%ld\n",
                            long(v[0]), long(v[1]), long(v[2]));
        }
        Serial.write(reinterpret_cast<const uint8_t*>(out_.data()), len);
    }
    void marker(uint64_t, const String&) override {}
    void close() override {}

private:
    std::vector<char> out_;
};

logger::Logger app("bench", "TinyS3[D]", FIRMWARE_VERSION);
logger::Adxl355 accel("accel", ADXL355_PINS);
TextSink text;
logger::SdCard sd(SD_PINS);
logger::WifiFeature wifi;  // hotspot + status page at http://192.168.4.1
logger::BatteryMax17048 battery;  // onboard fuel gauge, I2C on IO8/IO9

void setup() {
    app.led().setPins(PIN_LED_STATUS, PIN_LED_POWER);
    app.add(accel);
    app.add(battery);
    app.add(sd);
    app.add(wifi);
    app.addSink(text);
    app.addSink(sd);
    app.begin(CONFIG);
}

void loop() {
    app.loop();

    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        const logger::StreamInfo& s = *accel.stream(0);
        char line[128];
        snprintf(line, sizeof(line), "#ADXL355 rate=%g g_per_lsb=%.9f raw_max=%ld\n",
                 s.rateHz, s.channels[0].scale, long(s.rawMax));
        app.print(line);
        if (!accel.ok()) app.print("#ERROR ADXL355 not responding (ID or settings wrong). Check wiring.\n");
        if (battery.present()) {
            snprintf(line, sizeof(line), "#BATTERY voltage=%.3f percent=%.1f usb=%d charging=%d\n",
                     battery.voltage(), battery.percent(), battery.usbPower(), battery.charging());
            app.print(line);
        }
        JsonDocument d;
        sd.status(d.to<JsonObject>());
        snprintf(line, sizeof(line), "#SD present=%d free_mb=%lu total_mb=%lu file=%s\n",
                 int(d["present"].as<bool>()), (unsigned long)d["free_mb"].as<uint32_t>(),
                 (unsigned long)d["total_mb"].as<uint32_t>(), d["file"] | "-");
        app.print(line);
    }
}
