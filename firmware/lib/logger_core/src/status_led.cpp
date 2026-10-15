#include "status_led.h"

namespace logger {

namespace {
constexpr int PIN_RGB = 18;        // TinyS3[D] onboard RGB LED
constexpr int PIN_RGB_POWER = 17;  // its power switch
}  // namespace

void StatusLed::begin(float brightness) {
    setBrightness(brightness);
    pinMode(PIN_RGB_POWER, OUTPUT);
    digitalWrite(PIN_RGB_POWER, HIGH);
    rgb(0, 0, 0);
}

void StatusLed::rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t packed = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (packed == last_) return;  // avoid re-sending the same colour every loop
    last_ = packed;
    neopixelWrite(PIN_RGB, uint8_t(r * brightness_), uint8_t(g * brightness_), uint8_t(b * brightness_));
}

void StatusLed::update() {
    const uint32_t t = millis();
    auto blink = [&](uint32_t period, uint32_t on) { return (t % period) < on; };

    if (int32_t(identifyUntil_ - t) > 0) {
        blink(150, 75) ? rgb(255, 255, 255) : rgb(0, 0, 0);
        return;
    }
    switch (pattern_) {
        case LedPattern::Off:
            rgb(0, 0, 0);
            break;
        case LedPattern::Countdown: {
            uint32_t period = countdownMs_ < 3000 ? 250 : 1000;  // faster in the last 3 s
            blink(period, period / 2) ? rgb(0, 0, 255) : rgb(0, 0, 0);
            break;
        }
        case LedPattern::Recording:
            blink(3000, 80) ? rgb(0, 255, 0) : rgb(0, 0, 0);
            break;
        case LedPattern::Connected:
            blink(3000, 80) ? rgb(0, 255, 255) : rgb(0, 0, 0);
            break;
        case LedPattern::Idle: {
            // slow triangle-wave pulse, 4 s period
            uint32_t ph = t % 4000;
            uint8_t v = uint8_t((ph < 2000 ? ph : 4000 - ph) * 255 / 2000 / 4);
            rgb(v, v, v);
            break;
        }
        case LedPattern::Warning:
            blink(1000, 150) ? rgb(255, 160, 0) : rgb(0, 0, 0);
            break;
        case LedPattern::Error:
            blink(300, 150) ? rgb(255, 0, 0) : rgb(0, 0, 0);
            break;
        case LedPattern::Identify:
            break;  // handled above
    }
}

}  // namespace logger
