// Bench test: ADXL355 streamed as text for tools/accel-viewer (temporary).
//
// Uses the real Adxl355 feature and the Logger's acquisition/writer pipeline, with a text sink
// in place of SD/USB DLOG, so the viewer exercises the same driver the vibration logger records
// with. No SD card needed.
//
// Serial protocol (the viewer's):
//   #ADXL355 rate=<Hz> g_per_lsb=<scale> raw_max=<counts>   info line, every 2 s
//   #ERROR <message>                                         sensor problem, every 2 s while it lasts
//   D,<x>,<y>,<z>                                            one sample, raw counts
// The usual bench console commands (status, info, {"cmd":...}) also work; replies start with '#'.

#include <adxl355.h>
#include <logger.h>

#include <vector>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "bench"
#endif

// Same wiring as the vibration logger (docs/PLAN.md, Pin map)
static const logger::Adxl355::Pins ADXL355_PINS = {
    /*sck*/ 36, /*mosi*/ 35, /*miso*/ 37, /*cs*/ 34, /*drdy*/ 7};

static const char* CONFIG = R"json({
  "recording": { "autostart": true, "start_delay_s": 2, "sinks": ["text"] },
  "accel":     { "range_g": 8, "odr_hz": 4000, "impl": { "spi_hz": 5000000 } }
})json";

// Prints every frame as a "D,x,y,z" line. Runs in the Logger's writer task.
class TextSink : public logger::Sink {
public:
    const char* name() const override { return "text"; }
    bool open(const String&, const String&, uint32_t, uint32_t) override { return true; }
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

void setup() {
    app.add(accel);
    app.addSink(text);
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
    }
}
