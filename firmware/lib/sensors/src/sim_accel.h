// Simulated accelerometer implementing the `accel` feature (protocol/features/accel.json).
// Produces known sine waves (plus noise, and optionally injected gaps and clipping) with the
// same stream format as the ADXL355, so the whole data path can be tested without hardware.
//
// Config (accel section):  range_g, odr_hz, mounting, and impl:
//   "impl": { "tones": [ {"axis": 0, "freq_hz": 50, "amp_g": 0.5}, … ],
//             "noise_g": 0.002, "gap_every_s": 0, "clip_every_s": 0 }
#pragma once

#include <logger.h>

#include <vector>

namespace logger {

class SimAccel : public Feature {
public:
    explicit SimAccel(const char* instance = "accel") : Feature("accel", instance, 1) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void describe(JsonObject info) const override;
    void status(JsonObject out) const override;
    bool hasCommand(const char* cmd) const override { return strcmp(cmd, "self_test") == 0; }
    CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) override;

    size_t streamCount() const override { return 1; }
    StreamInfo* stream(size_t i) override { return i == 0 ? &info_ : nullptr; }
    void streamMetadata(size_t i, JsonObject out) const override;

    void startAcquisition() override;
    void stopAcquisition() override { running_ = false; }
    void acquire() override;

private:
    struct Tone {
        uint8_t axis;
        float freqHz;
        float ampG;
    };
    static constexpr int32_t RAW_MAX = 524287;  // 20-bit, like the ADXL355

    void emitBlock();
    float noise();

    Logger* host_ = nullptr;
    StreamInfo info_;
    float rangeG_ = 8;
    float odrHz_ = 4000;
    double gPerLsb_ = 8.0 / 512000;
    std::vector<Tone> tones_;
    float noiseG_ = 0.002f;
    float gapEveryS_ = 0;
    float clipEveryS_ = 0;
    JsonDocument mounting_;

    bool running_ = false;
    bool started_ = false;
    uint64_t t0_ = 0;          // device time of frame 0 (first start this boot)
    int64_t produced_ = 0;     // next frame index
    uint32_t blockFrames_ = 400;
    std::vector<int32_t> buf_;
    uint32_t rng_ = 0x12345678;

    uint64_t frames_ = 0;
    uint32_t gaps_ = 0;
    uint32_t clippedBlocks_ = 0;
};

}  // namespace logger
