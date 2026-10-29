// Analog Devices ADXL355 (EVAL-ADXL355-PMDZ) implementing the `accel` feature
// (protocol/features/accel.json). 20-bit, ±2/4/8 g, up to 4000 Hz, read from its 96-entry FIFO
// over SPI by the acquisition task.
//
// Robustness (lessons from the ADXL345 breadboard):
//   - every 2 s the device ID and configuration registers are read back; if anything is wrong
//     the sensor is reset and reconfigured, and the affected block is flagged SUSPECT
//   - a FIFO overflow resynchronises the frame counter from the clock and flags the next block GAP
//   - samples at full scale flag the block CLIPPED
//
// Config (accel section): range_g (2/4/8), odr_hz (125…4000), mounting, and
//   "impl": { "spi_hz": 5000000 }
#pragma once

#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <logger.h>

#include <vector>

namespace logger {

class Adxl355 : public Feature {
public:
    struct Pins {
        int sck, mosi, miso, cs, drdy;
    };
    Adxl355(const char* instance, const Pins& pins) : Feature("accel", instance, 1), pins_(pins), spi_(HSPI) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void describe(JsonObject info) const override;
    void status(JsonObject out) const override;
    bool ok() const override { return ok_; }
    bool hasCommand(const char* cmd) const override { return strcmp(cmd, "self_test") == 0; }
    CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) override;

    size_t streamCount() const override { return 1; }
    StreamInfo* stream(size_t i) override { return i == 0 ? &info_ : nullptr; }
    void streamMetadata(size_t i, JsonObject out) const override;

    void startAcquisition() override;
    void stopAcquisition() override;
    void acquire() override;

private:
    static constexpr int32_t RAW_MAX = 524287;  // 20-bit two's complement
    static constexpr int32_t RAW_MIN = -524288;

    // All need lock_ held
    bool configure();
    bool configOk();
    uint8_t readReg(uint8_t reg);
    void readRegs(uint8_t reg, uint8_t* buf, size_t n);
    void writeReg(uint8_t reg, uint8_t value);
    void drainFifo();
    void healthCheck();
    void readFifo();
    void appendFrame(const int32_t* xyz, uint64_t tMono);
    void flushBlock();
    void readAverage(float out[3], int samples);

    Logger* host_ = nullptr;
    Pins pins_;
    SPIClass spi_;
    SPISettings spiSettings_;
    SemaphoreHandle_t lock_ = nullptr;

    StreamInfo info_;
    float rangeG_ = 8;
    float odrHz_ = 4000;
    uint8_t rangeCode_ = 3;
    uint8_t odrCode_ = 0;
    double gPerLsb_ = 8.0 / 512000;
    JsonDocument mounting_;

    volatile bool ok_ = false;
    volatile bool running_ = false;
    bool started_ = false;
    uint64_t t0_ = 0;
    uint64_t frameIndex_ = 0;  // next frame's index
    uint32_t lastHealthMs_ = 0;
    float temperatureC_ = NAN;

    uint32_t blockFrames_ = 400;
    std::vector<int32_t> block_;
    uint32_t blockN_ = 0;
    uint64_t blockFirst_ = 0;
    uint64_t blockT_ = 0;
    uint16_t blockFlags_ = 0;
    uint16_t pendingFlags_ = 0;  // applied to the next block (GAP after an overflow, …)
    uint8_t fifoBuf_[96 * 3];

    uint64_t frames_ = 0;
    uint32_t gaps_ = 0;
    uint32_t overflows_ = 0;
    uint32_t clippedBlocks_ = 0;
    uint32_t errors_ = 0;
    uint32_t resets_ = 0;
    uint8_t lastId_[3] = {0, 0, 0};  // ID bytes from the last probe, for wiring diagnosis
};

}  // namespace logger
