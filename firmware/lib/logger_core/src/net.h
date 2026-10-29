// The Logger's link to the master (protocol/MQTT.md). Implemented by lib/net's MqttLink; loggers
// without a network simply don't have one, and nothing in the core requires it.
//
// Called from the main loop only.
#pragma once

#include <Arduino.h>

namespace logger {

class Logger;

class Net {
public:
    virtual ~Net() = default;
    virtual void begin(Logger& host) = 0;
    virtual void poll() = 0;
    virtual bool connected() const = 0;  // to the master
    virtual bool linkUp() const = 0;     // on a network that should lead to the master (may still be connecting)

    // Publish JSON on loggers/<device_id>/<subtopic>. Never blocks; false if not connected.
    virtual bool publish(const String& subtopic, const String& json, int qos = 1, bool retain = false) = 0;

    // Re-announce info/config/status now (after a config change, …)
    virtual void announce() = 0;

    // Something in status changed that's worth sending before the next interval (rate-limited)
    virtual void statusSoon() {}

    // Start a network time sync now; false if there's no time server to ask
    virtual bool syncTime() = 0;
};

}  // namespace logger
