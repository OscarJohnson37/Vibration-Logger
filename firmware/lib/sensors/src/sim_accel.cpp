#include "sim_accel.h"

#include <math.h>

namespace logger {

namespace {
constexpr float ODR_CHOICES[] = {125, 250, 500, 1000, 2000, 4000};  // ADXL355 rates
constexpr float RANGE_CHOICES[] = {2, 4, 8};
}  // namespace

bool SimAccel::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;

    float range = config["range_g"] | 8.0f;
    rangeG_ = 8;
    for (float r : RANGE_CHOICES)
        if (r == range) rangeG_ = r;
    float odr = config["odr_hz"] | 4000.0f;
    odrHz_ = 4000;
    for (float o : ODR_CHOICES)
        if (o == odr) odrHz_ = o;
    gPerLsb_ = rangeG_ / 512000.0;  // ADXL355: 256000 LSB/g at +/-2 g
    blockFrames_ = max(1, int(odrHz_ / 10));  // 0.1 s blocks
    buf_.resize(size_t(blockFrames_) * 3);

    JsonObjectConst impl = config["impl"];
    tones_.clear();
    JsonArrayConst tones = impl["tones"];
    if (tones.isNull()) {
        tones_ = {{0, 50, 0.5f}, {1, 120, 0.1f}, {1, 25, 0.2f}, {2, 0, 1.0f}};  // z = gravity
    } else {
        for (JsonObjectConst t : tones)
            tones_.push_back({uint8_t(constrain(t["axis"] | 0, 0, 2)), t["freq_hz"] | 0.0f, t["amp_g"] | 0.0f});
    }
    noiseG_ = impl["noise_g"] | 0.002f;
    gapEveryS_ = impl["gap_every_s"] | 0.0f;
    clipEveryS_ = impl["clip_every_s"] | 0.0f;
    mounting_.set(config["mounting"]);

    info_.name = instance();
    info_.feature = instance();
    info_.rateHz = odrHz_;
    info_.dtype = Dtype::I24;
    info_.hasRawRange = true;
    info_.rawMin = -RAW_MAX - 1;
    info_.rawMax = RAW_MAX;
    info_.nChannels = 3;
    const char* names[] = {"x", "y", "z"};
    for (int c = 0; c < 3; c++) {
        info_.channels[c].name = names[c];
        info_.channels[c].unit = "g";
        info_.channels[c].scale = gPerLsb_;
        info_.channels[c].offset = 0.0;
    }
    return true;
}

void SimAccel::describe(JsonObject info) const {
    info["impl"] = "sim";
    JsonObject limits = info["limits"].to<JsonObject>();
    JsonArray r = limits["range_g"].to<JsonArray>();
    for (float v : RANGE_CHOICES) r.add(v);
    JsonArray o = limits["odr_hz"].to<JsonArray>();
    for (float v : ODR_CHOICES) o.add(v);
}

void SimAccel::status(JsonObject out) const {
    out["ok"] = true;
    out["frames"] = frames_;
    out["gaps"] = gaps_;
    out["clipped_blocks"] = clippedBlocks_;
    out["errors"] = 0;
}

void SimAccel::streamMetadata(size_t, JsonObject out) const {
    JsonObject s = out["sensor"].to<JsonObject>();
    s["model"] = "sim";
    s["range_g"] = rangeG_;
    s["odr_hz"] = odrHz_;
    JsonArray tones = s["tones"].to<JsonArray>();
    for (const Tone& t : tones_) {
        JsonObject o = tones.add<JsonObject>();
        o["axis"] = t.axis;
        o["freq_hz"] = t.freqHz;
        o["amp_g"] = t.ampG;
    }
    s["noise_g"] = noiseG_;
    if (!mounting_.isNull()) out["mounting"] = mounting_;
}

CmdStatus SimAccel::command(const char* cmd, JsonObjectConst, JsonObject result, String& message) {
    if (strcmp(cmd, "self_test") != 0) return CmdStatus::Unsupported;
    if (running_) {
        message = "stop recording first";
        return CmdStatus::Busy;
    }
    result["passed"] = true;
    return CmdStatus::Ok;
}

void SimAccel::startAcquisition() {
    uint64_t now = host_->monoUs();
    if (!started_) {
        t0_ = now;  // frame indices count from the first start this boot
        produced_ = 0;
        started_ = true;
    } else {
        // Frames between recordings weren't produced: skip ahead so the reader sees the gap
        produced_ = int64_t(double(now - t0_) * odrHz_ / 1e6);
    }
    running_ = true;
}

void SimAccel::acquire() {
    if (!running_) return;
    int64_t due = int64_t(double(host_->monoUs() - t0_) * odrHz_ / 1e6);
    while (running_ && due - produced_ >= int64_t(blockFrames_)) emitBlock();
}

float SimAccel::noise() {
    // xorshift32 -> roughly uniform in [-1, 1), scaled to the noise amplitude
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return noiseG_ * (float(rng_) / 2147483648.0f - 1.0f) * 1.732f;
}

void SimAccel::emitBlock() {
    uint16_t flags = 0;

    // Optional injected gap: drop 20 ms of frames every gap_every_s
    if (gapEveryS_ > 0) {
        int64_t every = int64_t(gapEveryS_ * odrHz_);
        if (every > 0 && produced_ > 0 && produced_ / every != (produced_ + blockFrames_) / every) {
            produced_ += int64_t(odrHz_ * 0.02f);
            flags |= FLAG_GAP;
            gaps_++;
        }
    }

    const int64_t first = produced_;
    const int64_t clipEvery = clipEveryS_ > 0 ? int64_t(clipEveryS_ * odrHz_) : 0;
    for (uint32_t k = 0; k < blockFrames_; k++) {
        const int64_t frame = first + k;
        const double t = double(frame) / odrHz_;
        float g[3] = {noise(), noise(), noise()};
        for (const Tone& tone : tones_)
            g[tone.axis] += tone.freqHz > 0 ? tone.ampG * float(sin(2.0 * M_PI * tone.freqHz * t)) : tone.ampG;
        if (clipEvery > 0 && frame % clipEvery < int64_t(odrHz_ * 0.005f)) g[0] += 2 * rangeG_;  // 5 ms shock
        for (int a = 0; a < 3; a++) {
            int32_t counts = int32_t(lround(g[a] / gPerLsb_));
            if (counts >= RAW_MAX || counts <= -RAW_MAX - 1) {
                counts = counts > 0 ? RAW_MAX : -RAW_MAX - 1;
                flags |= FLAG_CLIPPED;
            }
            buf_[k * 3 + a] = counts;
        }
    }
    if (flags & FLAG_CLIPPED) clippedBlocks_++;

    SampleBlock b;
    b.stream = &info_;
    b.flags = flags;
    b.nFrames = blockFrames_;
    b.firstFrame = uint64_t(first);
    b.tMonoUs = t0_ + uint64_t(double(first) * 1e6 / odrHz_);
    b.i32 = buf_.data();
    host_->submit(b);
    produced_ += blockFrames_;
    frames_ += blockFrames_;
}

}  // namespace logger
