#include "status_led.h"

namespace logger {

namespace {
constexpr int PIN_RGB = 18;        // TinyS3[D] onboard RGB LED (mirror)
constexpr int PIN_RGB_POWER = 17;  // its power switch
constexpr uint8_t CH_STATUS = 0, CH_POWER = 1;  // LEDC channels for the case LEDs

bool blink(uint32_t t, uint32_t period, uint32_t on) { return (t % period) < on; }

// n short flashes, spacing ms apart, at the start of every period
bool pulses(uint32_t t, uint32_t period, int n, uint32_t spacing = 250) {
    uint32_t ph = t % period;
    return ph < n * spacing && ph % spacing < 80;
}

// triangle wave, 0-255-0 over the period
uint8_t breathe(uint32_t t, uint32_t period) {
    uint32_t ph = t % period, half = period / 2;
    return uint8_t((ph < half ? ph : period - ph) * 255 / half);
}

void attach(int pin, uint8_t channel) {
    if (pin < 0) return;
    ledcSetup(channel, 5000, 8);
    ledcAttachPin(pin, channel);
    ledcWrite(channel, 0);
}
}  // namespace

void StatusLed::begin(float brightness) {
    setBrightness(brightness);
    attach(statusPin_, CH_STATUS);
    attach(powerPin_, CH_POWER);
    pinMode(PIN_RGB_POWER, OUTPUT);
    digitalWrite(PIN_RGB_POWER, HIGH);
    write(0, 0);
}

void StatusLed::write(uint8_t status, uint8_t power) {
    uint32_t packed = (uint32_t(status) << 8) | power;
    if (packed == last_) return;  // avoid re-sending the same levels every loop
    last_ = packed;
    uint8_t s = uint8_t(status * brightness_), p = uint8_t(power * brightness_);
    if (statusPin_ >= 0) ledcWrite(CH_STATUS, s);
    if (powerPin_ >= 0) ledcWrite(CH_POWER, p);
    neopixelWrite(PIN_RGB, 0, s, p);
}

uint8_t StatusLed::statusLevel(uint32_t t) const {
    switch (pattern_) {
        case LedPattern::Countdown: {
            uint32_t period = countdownMs_ < 3000 ? 250 : 1000;  // faster in the last 3 s
            return blink(t, period, period / 2) ? 255 : 0;
        }
        case LedPattern::Recording: return pulses(t, 3000, 1) ? 255 : 0;
        case LedPattern::Connected: return pulses(t, 3000, 2) ? 255 : 0;
        case LedPattern::Idle: return breathe(t, 4000);
        case LedPattern::Warning: return blink(t, 1000, 150) ? 255 : 0;
        case LedPattern::Error: return blink(t, 300, 150) ? 255 : 0;
        default: return 0;
    }
}

uint8_t StatusLed::powerLevel(uint32_t t) const {
    switch (power_) {
        case PowerPattern::Single: return pulses(t, 4000, 1) ? 255 : 0;
        case PowerPattern::Double: return pulses(t, 4000, 2) ? 255 : 0;
        case PowerPattern::Fast: return pulses(t, 2000, 5, 150) ? 255 : 0;
        case PowerPattern::Charging: return breathe(t, 3000);
        case PowerPattern::Full: return 255;
        default: return 0;
    }
}

void StatusLed::update() {
    const uint32_t t = millis();
    if (int32_t(identifyUntil_ - t) > 0) {
        uint8_t v = blink(t, 150, 75) ? 255 : 0;
        write(v, v);
        return;
    }
    write(statusLevel(t), powerLevel(t));
}

}  // namespace logger
