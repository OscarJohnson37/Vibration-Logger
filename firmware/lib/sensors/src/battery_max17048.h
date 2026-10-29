// Battery level from the MAX17048 fuel gauge on the TinyS3[D] (I2C 0x36 on IO8/IO9), implementing
// the `battery` feature (protocol/features/battery.json). USB power is sensed on IO33.
//
// Also records a slow `battery` stream (voltage, percent) so a deployment's battery life is in
// the data file, and drives the blue power LED from the battery level and charging state.
// Config (battery section): medium_pct, low_warning_pct, critical_pct, log_interval_s (0 = don't
// record).
#pragma once

#include <logger.h>

namespace logger {

class BatteryMax17048 : public Feature {
public:
    explicit BatteryMax17048(int sda = 8, int scl = 9, int vbusSensePin = 33)
        : Feature("battery", "battery", 1), sda_(sda), scl_(scl), vbusPin_(vbusSensePin) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void describe(JsonObject info) const override { info["impl"] = "MAX17048"; }
    void status(JsonObject out) const override;
    bool ok() const override { return true; }  // a missing gauge isn't a reason to stop recording

    size_t streamCount() const override { return logIntervalS_ > 0 ? 1 : 0; }
    StreamInfo* stream(size_t i) override { return i == 0 && logIntervalS_ > 0 ? &info_ : nullptr; }
    void streamMetadata(size_t i, JsonObject out) const override;
    void startAcquisition() override;

    void poll() override;

    bool present() const { return present_; }
    float voltage() const { return voltage_; }
    float percent() const { return percent_; }
    bool usbPower() const { return usbPower_; }
    bool charging() const { return usbPower_ && chargeRate_ > 0.1f; }

    // Battery bands (also the power LED patterns): high > medium_pct > medium > low_warning_pct
    // > low > critical_pct > critical
    enum class Level : uint8_t { Unknown, High, Medium, Low, Critical };
    Level level() const { return level_; }
    bool full() const { return full_; }

private:
    bool readReg(uint8_t reg, uint16_t& value);
    bool writeReg(uint8_t reg, uint16_t value);
    void read();
    void checkEstimate();
    Level classify(float pct) const;
    void updateFull();
    void raiseEvents();
    PowerPattern powerPattern() const;

    Logger* host_ = nullptr;
    int sda_, scl_, vbusPin_;
    StreamInfo info_;
    float mediumPct_ = 70;
    float lowWarningPct_ = 30;
    float criticalPct_ = 10;
    float logIntervalS_ = 10;

    bool present_ = false;
    float voltage_ = NAN;
    float percent_ = NAN;
    float chargeRate_ = 0;  // %/hour, positive while charging
    bool usbPower_ = false;
    Level level_ = Level::Unknown;
    bool reportedUsb_ = false;          // last values sent promptly (statusChanged)
    Level reportedLevel_ = Level::Unknown;
    float reportedPct_ = -100;
    bool full_ = false;
    uint32_t notFullMs_ = 0;  // last read where the "full" conditions weren't all met
    bool lowReported_ = false;
    bool criticalReported_ = false;

    uint32_t onBatterySince_ = 0;  // 0 while on USB
    bool estimateChecked_ = false; // once per spell on battery
    uint32_t quickStarts_ = 0;

    uint32_t lastReadMs_ = 0;
    uint32_t lastLogMs_ = 0;
    uint64_t frame_ = 0;
    bool logging_ = false;
};

}  // namespace logger
