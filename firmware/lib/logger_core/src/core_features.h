// The built-in features every logger has: core and recording (protocol/features/).
#pragma once

#include "feature.h"

namespace logger {

class CoreFeature : public Feature {
public:
    CoreFeature() : Feature("core", "core", 1) {}
    bool begin(Logger& host, JsonObjectConst config) override;
    void status(JsonObject out) const override;
    bool hasCommand(const char* cmd) const override;
    CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) override;
    void poll() override;

private:
    Logger* host_ = nullptr;
    uint32_t rebootAt_ = 0;
};

class RecordingFeature : public Feature {
public:
    RecordingFeature() : Feature("recording", "recording", 1) {}
    bool begin(Logger& host, JsonObjectConst config) override;
    void status(JsonObject out) const override;
    bool hasCommand(const char* cmd) const override;
    CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) override;
    void poll() override;

    bool countingDown() const { return counting_; }
    uint32_t countdownRemainingMs() const;

private:
    Logger* host_ = nullptr;
    bool autostart_ = true;
    uint32_t startDelayS_ = 10;
    uint32_t flushIntervalS_ = 5;
    uint32_t fileMaxMinutes_ = 60;
    uint32_t fileMaxMb_ = 256;
    bool counting_ = false;
    uint32_t countdownUntil_ = 0;  // millis() deadline while counting_
    uint32_t lastFlush_ = 0;
};

}  // namespace logger
