// Base class for every feature (protocol/features/). A logger is a set of feature instances;
// the Logger host routes config, commands, status and samples to them by instance name.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "stream_info.h"

namespace logger {

class Logger;

// Command outcomes, matching the MQTT ack error codes (MQTT.md, Commands)
enum class CmdStatus { Ok, Unsupported, UnknownTarget, AmbiguousTarget, InvalidArgs, Busy, Failed };
const char* cmdStatusCode(CmdStatus s);

// Removes every key containing "password" (recursively): for published config and DLOG metadata
void stripSecrets(JsonVariant v);

class Feature {
public:
    Feature(const char* feature, const char* instance, int version)
        : feature_(feature), instance_(instance), version_(version) {}
    virtual ~Feature() = default;

    const char* feature() const { return feature_; }
    const char* instance() const { return instance_; }
    int version() const { return version_; }

    // Called once at start-up with this instance's config section (may be empty).
    virtual bool begin(Logger& host, JsonObjectConst config) { return true; }

    // Extra fields for this feature's entry in MQTT info: impl, limits, …
    virtual void describe(JsonObject info) const {}

    // This instance's status section (MQTT status.features.<instance>)
    virtual void status(JsonObject out) const {}

    // false = this instance can't do its job (sensor missing, …): the logger shows the error pattern
    virtual bool ok() const { return true; }

    // Commands this feature provides (per its definition)
    virtual bool hasCommand(const char* cmd) const { return false; }
    virtual CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) {
        return CmdStatus::Unsupported;
    }

    // Streams produced by this instance (sensor features)
    virtual size_t streamCount() const { return 0; }
    virtual StreamInfo* stream(size_t i) { return nullptr; }
    // Extra per-stream metadata for DLOG/info: "sensor", "mounting", …
    virtual void streamMetadata(size_t i, JsonObject out) const {}

    // Recording started/stopped: sensors reset counters and start/stop producing blocks
    virtual void startAcquisition() {}
    virtual void stopAcquisition() {}

    // true = this feature needs sensor blocks even when not recording (live publications), so
    // the host keeps acquisition running from start-up
    virtual bool wantsSamples() const { return false; }

    // Called about every 1 ms from the high-priority acquisition task: read sensors here and
    // submit blocks. Must not block for long. Everything else belongs in poll().
    virtual void acquire() {}

    // Every block any feature submits, in the submitting task (usually acquisition): for
    // features that summarise data (publish). Must be quick and must not block.
    virtual void consume(const SampleBlock& b) {}

    // Called frequently from the main loop (housekeeping, policy, slow sensors)
    virtual void poll() {}

private:
    const char* feature_;
    const char* instance_;
    int version_;
};

}  // namespace logger
