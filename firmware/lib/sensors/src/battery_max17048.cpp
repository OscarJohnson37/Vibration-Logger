#include "battery_max17048.h"

#include <Wire.h>

namespace logger {

namespace {
constexpr uint8_t ADDR = 0x36;
constexpr uint8_t REG_VCELL = 0x02;    // 78.125 uV / LSB
constexpr uint8_t REG_SOC = 0x04;      // 1/256 % / LSB
constexpr uint8_t REG_VERSION = 0x08;  // 0x001x
constexpr uint8_t REG_CRATE = 0x16;    // signed, 0.208 %/hour / LSB
constexpr uint8_t REG_MODE = 0x06;
constexpr uint16_t MODE_QUICK_START = 0x4000;  // re-estimate the charge from the cell voltage now

// The gauge estimates the charge from the voltage when it powers up, then tracks it slowly. If it
// powered up on the charger's voltage (battery switched off, USB in) it believes the battery is
// full and takes hours to correct. So after this long on battery alone, when the voltage settles
// near the cell's resting voltage, an estimate this far from the voltage-based one is redone.
constexpr uint32_t SETTLE_MS = 20000;
constexpr float ESTIMATE_MAX_ERROR_PCT = 20;

// Typical LiPo resting voltage vs charge (light load, as here: ~0.1 A from a 3000 mAh cell)
float percentFromVoltage(float v) {
    static const float V[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f, 3.82f, 3.84f,
                              3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f};
    const int n = sizeof(V) / sizeof(V[0]);  // 0 %, 5 %, … 100 %
    if (v <= V[0]) return 0;
    if (v >= V[n - 1]) return 100;
    int i = 1;
    while (v > V[i]) i++;
    return 5 * (i - 1 + (v - V[i - 1]) / (V[i] - V[i - 1]));
}

// Moving down a band happens at its threshold; moving back up needs this much more, so a
// reading jittering around a threshold doesn't flip the LED or repeat events.
constexpr float HYSTERESIS_PCT = 3;

// The charger's "done" signal isn't wired to the ESP32, so "full" is inferred: on USB, near
// 100 % and the charge rate ~0 (charger stopped), held long enough to not flicker at the end.
constexpr float FULL_PCT = 95;
constexpr float FULL_MAX_RATE = 1.0f;  // %/hour
constexpr uint32_t FULL_HOLD_MS = 60000;

const char* levelName(BatteryMax17048::Level l) {
    switch (l) {
        case BatteryMax17048::Level::High: return "high";
        case BatteryMax17048::Level::Medium: return "medium";
        case BatteryMax17048::Level::Low: return "low";
        case BatteryMax17048::Level::Critical: return "critical";
        default: return "unknown";
    }
}
}  // namespace

bool BatteryMax17048::readReg(uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(int(ADDR), 2) != 2) return false;
    value = uint16_t(Wire.read()) << 8;
    value |= uint16_t(Wire.read());
    return true;
}

bool BatteryMax17048::writeReg(uint8_t reg, uint16_t value) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(uint8_t(value >> 8));
    Wire.write(uint8_t(value));
    return Wire.endTransmission() == 0;
}

void BatteryMax17048::checkEstimate() {
    if (!usbPower_ && !onBatterySince_) onBatterySince_ = lastReadMs_ ? lastReadMs_ : 1;
    if (usbPower_) {
        onBatterySince_ = 0;
        estimateChecked_ = false;  // check again next time it's on battery
        return;
    }
    if (estimateChecked_ || lastReadMs_ - onBatterySince_ < SETTLE_MS || isnan(voltage_) || isnan(percent_)) return;
    estimateChecked_ = true;
    const float fromVoltage = percentFromVoltage(voltage_);
    if (fabsf(percent_ - fromVoltage) < ESTIMATE_MAX_ERROR_PCT) return;
    if (!writeReg(REG_MODE, MODE_QUICK_START)) return;
    quickStarts_++;
    JsonDocument d;
    d["percent_before"] = percent_;
    d["voltage"] = voltage_;
    d["percent_from_voltage"] = fromVoltage;
    host_->event("gauge_reset", instance(), d.as<JsonObjectConst>());
    delay(5);
    uint16_t soc;
    if (readReg(REG_SOC, soc)) percent_ = min(soc / 256.0f, 100.0f);  // fresh estimate, straight away
    level_ = Level::Unknown;  // re-band from scratch, no hysteresis from the wrong value
    level_ = classify(percent_);
}

bool BatteryMax17048::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    mediumPct_ = config["medium_pct"] | 70.0f;
    lowWarningPct_ = config["low_warning_pct"] | 30.0f;
    criticalPct_ = config["critical_pct"] | 10.0f;
    logIntervalS_ = config["log_interval_s"] | 10.0f;
    if (vbusPin_ >= 0) pinMode(vbusPin_, INPUT);
    Wire.begin(sda_, scl_);

    uint16_t version = 0;
    present_ = readReg(REG_VERSION, version) && (version & 0xFFF0) == 0x0010;

    info_.name = "battery";
    info_.feature = instance();
    info_.rateHz = logIntervalS_ > 0 ? 1.0 / logIntervalS_ : 0;
    info_.dtype = Dtype::F32;
    info_.nChannels = 2;
    info_.channels[0].name = "voltage";
    info_.channels[0].unit = "V";
    info_.channels[1].name = "percent";
    info_.channels[1].unit = "%";
    read();
    return present_;
}

void BatteryMax17048::read() {
    lastReadMs_ = millis();
    usbPower_ = vbusPin_ >= 0 && digitalRead(vbusPin_);
    if (present_) {
        uint16_t v, soc, crate;
        if (readReg(REG_VCELL, v)) voltage_ = v * 78.125e-6f;
        if (readReg(REG_SOC, soc)) percent_ = min(soc / 256.0f, 100.0f);
        if (readReg(REG_CRATE, crate)) chargeRate_ = int16_t(crate) * 0.208f;
        if (!isnan(percent_)) level_ = classify(percent_);
        checkEstimate();
        updateFull();
        raiseEvents();
    }
    // Worth showing on the dashboard now rather than at the next status interval
    if (usbPower_ != reportedUsb_ || level_ != reportedLevel_ || fabsf(percent_ - reportedPct_) >= 1) {
        reportedUsb_ = usbPower_;
        reportedLevel_ = level_;
        reportedPct_ = percent_;
        host_->statusChanged();
    }
    host_->led().setPower(powerPattern());
}

BatteryMax17048::Level BatteryMax17048::classify(float pct) const {
    const float thresholds[3] = {mediumPct_, lowWarningPct_, criticalPct_};  // below each: next band
    int current = level_ == Level::Unknown ? 0 : int(level_) - int(Level::High);
    int band = 0;
    for (int i = 0; i < 3; i++) {
        float t = thresholds[i] + (current > i ? HYSTERESIS_PCT : 0);  // already below: harder to leave
        if (pct < t) band = i + 1;
    }
    return Level(int(Level::High) + band);
}

void BatteryMax17048::updateFull() {
    bool conditions = usbPower_ && percent_ >= FULL_PCT && fabsf(chargeRate_) < FULL_MAX_RATE;
    if (!conditions) notFullMs_ = lastReadMs_;
    full_ = conditions && lastReadMs_ - notFullMs_ >= FULL_HOLD_MS;
}

// Once per discharge each; re-armed when the level recovers (it's been charged)
void BatteryMax17048::raiseEvents() {
    auto raise = [&](const char* name, bool& reported) {
        if (reported || usbPower_) return;
        JsonDocument d;
        d["percent"] = percent_;
        host_->event(name, instance(), d.as<JsonObjectConst>());
        reported = true;
    };
    if (level_ >= Level::Low) raise("battery_low", lowReported_);
    else lowReported_ = false;
    if (level_ == Level::Critical) raise("battery_critical", criticalReported_);
    else criticalReported_ = false;
}

PowerPattern BatteryMax17048::powerPattern() const {
    if (!present_) return PowerPattern::Off;
    if (usbPower_) return full_ ? PowerPattern::Full : PowerPattern::Charging;
    switch (level_) {
        case Level::Medium: return PowerPattern::Single;
        case Level::Low: return PowerPattern::Double;
        case Level::Critical: return PowerPattern::Fast;
        default: return PowerPattern::Off;
    }
}

void BatteryMax17048::startAcquisition() {
    logging_ = logIntervalS_ > 0;
    lastLogMs_ = millis() - uint32_t(logIntervalS_ * 1000);  // log one straight away
}

void BatteryMax17048::poll() {
    uint32_t now = millis();
    if (now - lastReadMs_ >= 5000) read();
    if (logging_ && host_->recording() && present_ && now - lastLogMs_ >= uint32_t(logIntervalS_ * 1000)) {
        lastLogMs_ = now;
        float values[2] = {voltage_, percent_};
        SampleBlock b;
        b.stream = &info_;
        b.nFrames = 1;
        b.firstFrame = frame_++;
        b.tMonoUs = host_->monoUs();
        b.f32 = values;
        host_->submit(b);
    }
}

void BatteryMax17048::status(JsonObject out) const {
    out["present"] = present_;
    if (!isnan(voltage_)) out["voltage"] = voltage_;
    if (!isnan(percent_)) out["percent"] = percent_;
    out["level"] = levelName(level_);
    out["charge_rate_pct_per_h"] = chargeRate_;
    out["charging"] = charging();
    out["full"] = full_;
    out["usb_power"] = usbPower_;
    if (quickStarts_) out["gauge_resets"] = quickStarts_;
}

void BatteryMax17048::streamMetadata(size_t, JsonObject out) const {
    out["sensor"]["model"] = "MAX17048";
}

}  // namespace logger
