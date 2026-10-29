// Status LEDs with the same patterns on every logger (docs/PLAN.md, Status LEDs): two
// single-colour LEDs in the case wall, green for logger status and blue for power (battery and
// charging). The TinyS3[D]'s onboard RGB LED mirrors them on its green and blue channels for
// bench use. Loggers without a battery leave the power LED unwired (pin -1) and off.
#pragma once

#include <Arduino.h>

namespace logger {

// Green LED, set by the Logger
enum class LedPattern {
    Off,
    Countdown,   // blinking, faster near the end
    Recording,   // short pulse every few seconds
    Connected,   // double pulse: recording and connected to the master
    Idle,        // slow breathing
    Warning,     // 1 Hz short blink
    Error,       // fast blink
    Identify,    // rapid flashing (both LEDs)
};

// Blue LED, set by the battery feature
enum class PowerPattern {
    Off,       // on battery and above battery.medium_pct, or no fuel gauge
    Single,    // short pulse: medium
    Double,    // double pulse: low
    Fast,      // burst of fast flashes: critical
    Charging,  // slow breathing: on USB, charging
    Full,      // steady: on USB, charged
};

class StatusLed {
public:
    void setPins(int statusPin, int powerPin) { statusPin_ = statusPin; powerPin_ = powerPin; }
    void begin(float brightness);
    void set(LedPattern p) { pattern_ = p; }
    void setPower(PowerPattern p) { power_ = p; }
    void setCountdown(uint32_t remainingMs) { countdownMs_ = remainingMs; }
    void identify(uint32_t seconds) { identifyUntil_ = millis() + seconds * 1000; }
    void setBrightness(float b) { brightness_ = constrain(b, 0.0f, 1.0f); }
    void update();

private:
    uint8_t statusLevel(uint32_t t) const;
    uint8_t powerLevel(uint32_t t) const;
    void write(uint8_t status, uint8_t power);

    int statusPin_ = -1, powerPin_ = -1;
    LedPattern pattern_ = LedPattern::Off;
    PowerPattern power_ = PowerPattern::Off;
    float brightness_ = 0.2f;
    uint32_t countdownMs_ = 0;
    uint32_t identifyUntil_ = 0;
    uint32_t last_ = 0xFFFFFFFF;
};

}  // namespace logger
