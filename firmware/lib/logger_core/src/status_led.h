// Status LED with the same colour codes on every logger (docs/PLAN.md, Status LED).
// Drives the TinyS3[D]'s onboard RGB LED for now; the case-mounted RGB LED is added in phase 3.
#pragma once

#include <Arduino.h>

namespace logger {

enum class LedPattern {
    Off,
    Countdown,   // blue blinking, faster near the end
    Recording,   // green short blink every few seconds
    Connected,   // cyan short blink: recording and connected to the master
    Idle,        // slow white pulse
    Warning,     // yellow blink
    Error,       // red fast blink
    Identify,    // rapid white flashing
};

class StatusLed {
public:
    void begin(float brightness);
    void set(LedPattern p) { pattern_ = p; }
    void setCountdown(uint32_t remainingMs) { countdownMs_ = remainingMs; }
    void identify(uint32_t seconds) { identifyUntil_ = millis() + seconds * 1000; }
    void setBrightness(float b) { brightness_ = constrain(b, 0.0f, 1.0f); }
    void update();

private:
    void rgb(uint8_t r, uint8_t g, uint8_t b);

    LedPattern pattern_ = LedPattern::Off;
    float brightness_ = 0.2f;
    uint32_t countdownMs_ = 0;
    uint32_t identifyUntil_ = 0;
    uint32_t last_ = 0xFFFFFFFF;
};

}  // namespace logger
