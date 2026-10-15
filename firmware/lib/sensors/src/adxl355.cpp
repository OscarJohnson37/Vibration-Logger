#include "adxl355.h"

#include <math.h>

namespace logger {

namespace {

// Registers (ADXL355 datasheet)
constexpr uint8_t REG_DEVID_AD = 0x00;      // 0xAD
constexpr uint8_t REG_STATUS = 0x04;
constexpr uint8_t REG_FIFO_ENTRIES = 0x05;
constexpr uint8_t REG_TEMP2 = 0x06;
constexpr uint8_t REG_XDATA3 = 0x08;
constexpr uint8_t REG_FIFO_DATA = 0x11;
constexpr uint8_t REG_FILTER = 0x28;
constexpr uint8_t REG_RANGE = 0x2C;
constexpr uint8_t REG_POWER_CTL = 0x2D;
constexpr uint8_t REG_SELF_TEST = 0x2E;
constexpr uint8_t REG_RESET = 0x2F;

constexpr uint8_t STATUS_FIFO_OVR = 0x04;
constexpr uint8_t RANGE_I2C_HS = 0x80;      // reset default; keep it set
constexpr uint8_t POWER_MEASURE = 0x00;     // measurement on, temperature on, DRDY on

struct Choice {
    float value;
    uint8_t code;
};
constexpr Choice ODRS[] = {{4000, 0}, {2000, 1}, {1000, 2}, {500, 3}, {250, 4}, {125, 5}};
constexpr Choice RANGES[] = {{2, 1}, {4, 2}, {8, 3}};

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }

private:
    SemaphoreHandle_t m_;
};

int32_t decode20(const uint8_t* b) {
    int32_t v = (int32_t(b[0]) << 12) | (int32_t(b[1]) << 4) | (b[2] >> 4);
    return (v & 0x80000) ? v - 0x100000 : v;
}

}  // namespace

// ------------------------------------------------------------------ SPI (lock_ held)

uint8_t Adxl355::readReg(uint8_t reg) {
    uint8_t v;
    readRegs(reg, &v, 1);
    return v;
}

void Adxl355::readRegs(uint8_t reg, uint8_t* buf, size_t n) {
    memset(buf, 0, n);
    spi_.beginTransaction(spiSettings_);
    digitalWrite(pins_.cs, LOW);
    spi_.transfer(uint8_t(reg << 1 | 1));
    spi_.transfer(buf, n);  // auto-increments, except FIFO_DATA which pops successive entries
    digitalWrite(pins_.cs, HIGH);
    spi_.endTransaction();
}

void Adxl355::writeReg(uint8_t reg, uint8_t value) {
    spi_.beginTransaction(spiSettings_);
    digitalWrite(pins_.cs, LOW);
    spi_.transfer(uint8_t(reg << 1));
    spi_.transfer(value);
    digitalWrite(pins_.cs, HIGH);
    spi_.endTransaction();
}

bool Adxl355::configure() {
    writeReg(REG_RESET, 0x52);
    delay(10);
    uint8_t id[3];
    readRegs(REG_DEVID_AD, id, 3);
    if (id[0] != 0xAD || id[1] != 0x1D || id[2] != 0xED) return false;
    writeReg(REG_RANGE, RANGE_I2C_HS | rangeCode_);
    writeReg(REG_FILTER, odrCode_);  // high-pass filter off; low-pass at ODR/4
    writeReg(REG_POWER_CTL, POWER_MEASURE);
    delay(5);
    drainFifo();
    return configOk();
}

bool Adxl355::configOk() {
    return readReg(REG_DEVID_AD) == 0xAD && (readReg(REG_RANGE) & 0x03) == rangeCode_ &&
           readReg(REG_FILTER) == odrCode_ && readReg(REG_POWER_CTL) == POWER_MEASURE;
}

void Adxl355::drainFifo() {
    for (int i = 0; i < 4; i++) {
        uint8_t n = readReg(REG_FIFO_ENTRIES) & 0x7F;
        if (!n) break;
        readRegs(REG_FIFO_DATA, fifoBuf_, n * 3);
    }
    readReg(REG_STATUS);  // clears FIFO_OVR
}

// ------------------------------------------------------------------ feature

bool Adxl355::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    lock_ = xSemaphoreCreateMutex();

    float range = config["range_g"] | 8.0f;
    rangeG_ = 8;
    rangeCode_ = 3;
    for (const Choice& c : RANGES)
        if (c.value == range) rangeG_ = c.value, rangeCode_ = c.code;
    float odr = config["odr_hz"] | 4000.0f;
    odrHz_ = 4000;
    odrCode_ = 0;
    for (const Choice& c : ODRS)
        if (c.value == odr) odrHz_ = c.value, odrCode_ = c.code;
    gPerLsb_ = rangeG_ / 512000.0;  // 256000 LSB/g at ±2 g
    blockFrames_ = max(1, int(odrHz_ / 10));  // 0.1 s blocks
    block_.assign(size_t(blockFrames_) * 3, 0);
    mounting_.set(config["mounting"]);

    uint32_t spiHz = constrain(config["impl"]["spi_hz"] | 5000000, 100000, 10000000);
    spiSettings_ = SPISettings(spiHz, MSBFIRST, SPI_MODE0);
    pinMode(pins_.cs, OUTPUT);
    digitalWrite(pins_.cs, HIGH);
    if (pins_.drdy >= 0) pinMode(pins_.drdy, INPUT);
    spi_.begin(pins_.sck, pins_.miso, pins_.mosi, pins_.cs);

    info_.name = instance();
    info_.feature = instance();
    info_.rateHz = odrHz_;
    info_.dtype = Dtype::I24;
    info_.hasRawRange = true;
    info_.rawMin = RAW_MIN;
    info_.rawMax = RAW_MAX;
    info_.nChannels = 3;
    const char* names[] = {"x", "y", "z"};
    for (int c = 0; c < 3; c++) {
        info_.channels[c].name = names[c];
        info_.channels[c].unit = "g";
        info_.channels[c].scale = gPerLsb_;
        info_.channels[c].offset = 0.0;
    }

    Lock l(lock_);
    ok_ = configure();
    if (!ok_) errors_++;
    lastHealthMs_ = millis();
    return ok_;
}

void Adxl355::describe(JsonObject info) const {
    info["impl"] = "ADXL355";
    JsonObject limits = info["limits"].to<JsonObject>();
    JsonArray r = limits["range_g"].to<JsonArray>();
    for (const Choice& c : RANGES) r.add(c.value);
    JsonArray o = limits["odr_hz"].to<JsonArray>();
    for (const Choice& c : ODRS) o.add(c.value);
}

void Adxl355::status(JsonObject out) const {
    out["ok"] = bool(ok_);
    out["frames"] = frames_;
    out["gaps"] = gaps_;
    out["fifo_overflows"] = overflows_;
    out["clipped_blocks"] = clippedBlocks_;
    out["errors"] = errors_;
    out["resets"] = resets_;
    if (!isnan(temperatureC_)) out["temperature_c"] = temperatureC_;
}

void Adxl355::streamMetadata(size_t, JsonObject out) const {
    JsonObject s = out["sensor"].to<JsonObject>();
    s["model"] = "ADXL355";
    s["range_g"] = rangeG_;
    s["odr_hz"] = odrHz_;
    s["lpf_hz"] = odrHz_ / 4;
    s["hpf"] = "off";
    if (!mounting_.isNull()) out["mounting"] = mounting_;
}

// ------------------------------------------------------------------ acquisition

void Adxl355::startAcquisition() {
    Lock l(lock_);
    uint64_t now = host_->monoUs();
    if (!started_) {
        t0_ = now;  // frame indices count from the first start this boot
        frameIndex_ = 0;
        started_ = true;
    } else {
        // Frames between recordings weren't kept: skip ahead so the reader sees the gap
        frameIndex_ = uint64_t(double(now - t0_) * odrHz_ / 1e6);
    }
    if (ok_) drainFifo();
    blockN_ = 0;
    pendingFlags_ = 0;
    running_ = true;
}

void Adxl355::stopAcquisition() {
    Lock l(lock_);
    flushBlock();  // keep the partial last block
    running_ = false;
}

void Adxl355::acquire() {
    Lock l(lock_);
    if (millis() - lastHealthMs_ >= 2000) {
        lastHealthMs_ = millis();
        healthCheck();
    }
    if (running_ && ok_) readFifo();
}

void Adxl355::healthCheck() {
    uint8_t t[2];
    readRegs(REG_TEMP2, t, 2);
    uint16_t raw = (uint16_t(t[0] & 0x0F) << 8) | t[1];
    if (configOk()) {
        temperatureC_ = (float(raw) - 1885.0f) / -9.05f + 25.0f;
        return;
    }
    // Wrong ID or settings: a glitch, loose wire or brown-out. Reset, and mark the data around it.
    errors_++;
    blockFlags_ |= FLAG_SUSPECT;
    pendingFlags_ |= FLAG_SUSPECT;
    bool wasOk = ok_;
    ok_ = configure();
    if (ok_) {
        resets_++;
        if (running_) {
            flushBlock();
            frameIndex_ = uint64_t(double(host_->monoUs() - t0_) * odrHz_ / 1e6);  // resync
            pendingFlags_ |= FLAG_GAP;
            gaps_++;
        }
    }
    if (wasOk && !ok_) host_->event("sensor_error", instance());
}

void Adxl355::readFifo() {
    if (readReg(REG_STATUS) & STATUS_FIFO_OVR) {
        // Samples were lost: finish the current block, then resync the counter from the clock
        overflows_++;
        gaps_++;
        flushBlock();
        uint64_t est = uint64_t(double(host_->monoUs() - t0_) * odrHz_ / 1e6);
        if (est > frameIndex_ + 32) frameIndex_ = est - 32;  // the FIFO still holds the newest 32 frames
        pendingFlags_ |= FLAG_GAP;
    }
    uint8_t entries = readReg(REG_FIFO_ENTRIES) & 0x7F;
    uint32_t nFrames = entries / 3;
    if (!nFrames) return;
    const uint64_t now = host_->monoUs();
    readRegs(REG_FIFO_DATA, fifoBuf_, nFrames * 9);

    const double periodUs = 1e6 / odrHz_;
    int32_t xyz[3];
    int axis = 0;
    uint32_t frame = 0;
    for (uint32_t e = 0; e < nFrames * 3; e++) {
        const uint8_t* b = fifoBuf_ + e * 3;
        if (b[2] & 0x02) break;               // empty entry
        bool isX = b[2] & 0x01;
        if (isX) axis = 0;
        else if (axis == 0) continue;         // out of step: wait for the next X entry
        xyz[axis++] = decode20(b);
        if (axis == 3) {
            // the newest frame was sampled just before 'now'; older ones one period apart
            appendFrame(xyz, now - uint64_t((nFrames - 1 - frame) * periodUs));
            frame++;
            axis = 0;
        }
    }
}

void Adxl355::appendFrame(const int32_t* xyz, uint64_t tMono) {
    if (blockN_ == 0) {
        blockFirst_ = frameIndex_;
        blockT_ = tMono;
        blockFlags_ = pendingFlags_;
        pendingFlags_ = 0;
    }
    for (int a = 0; a < 3; a++) {
        block_[blockN_ * 3 + a] = xyz[a];
        if (xyz[a] >= RAW_MAX || xyz[a] <= RAW_MIN) blockFlags_ |= FLAG_CLIPPED;
    }
    blockN_++;
    frameIndex_++;
    frames_++;
    if (blockN_ == blockFrames_) flushBlock();
}

void Adxl355::flushBlock() {
    if (!blockN_) return;
    if (blockFlags_ & FLAG_CLIPPED) clippedBlocks_++;
    SampleBlock b;
    b.stream = &info_;
    b.flags = blockFlags_;
    b.nFrames = blockN_;
    b.firstFrame = blockFirst_;
    b.tMonoUs = blockT_;
    b.i32 = block_.data();
    host_->submit(b);
    blockN_ = 0;
}

// ------------------------------------------------------------------ self-test

void Adxl355::readAverage(float out[3], int samples) {
    double sum[3] = {0, 0, 0};
    uint8_t b[9];
    for (int i = 0; i < samples; i++) {
        readRegs(REG_XDATA3, b, 9);
        for (int a = 0; a < 3; a++) sum[a] += decode20(b + 3 * a);
        delay(2);
    }
    for (int a = 0; a < 3; a++) out[a] = float(sum[a] / samples * gPerLsb_);
}

CmdStatus Adxl355::command(const char* cmd, JsonObjectConst, JsonObject result, String& message) {
    if (strcmp(cmd, "self_test") != 0) return CmdStatus::Unsupported;
    if (running_) {
        message = "stop recording first";
        return CmdStatus::Busy;
    }
    Lock l(lock_);
    if (!ok_) {
        message = "sensor not responding";
        return CmdStatus::Failed;
    }
    float off[3], on[3];
    readAverage(off, 50);
    writeReg(REG_SELF_TEST, 0x03);  // ST1 + ST2: apply the electrostatic test force
    delay(50);
    readAverage(on, 50);
    writeReg(REG_SELF_TEST, 0x00);
    delay(50);
    drainFifo();

    // Pass/fail criteria are provisional until checked against the datasheet limits on real units
    bool passed = true;
    JsonArray d = result["delta_g"].to<JsonArray>();
    for (int a = 0; a < 3; a++) {
        float delta = on[a] - off[a];
        d.add(delta);
        if (fabsf(delta) < 0.05f) passed = false;
    }
    result["passed"] = passed;
    result["criteria"] = "provisional: every axis moves > 0.05 g";
    return CmdStatus::Ok;
}

}  // namespace logger
