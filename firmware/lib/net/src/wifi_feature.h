// The `wifi` feature (protocol/features/wifi.json) and the logger-hosted web page.
//
// Joins the first of config "networks" it can find (the master's network first, then e.g. a
// bench network), retrying in the background. The logger's own hotspot (VL-<id>) runs while it
// isn't connected to the master ("no_master", the default, after a 20 s grace), while it hasn't
// joined a network ("no_network"), always, or never. Either way it serves:
//   /         a status page (battery, sensors, SD card, recording) that refreshes itself
//   /status   MQTT-style status JSON (MQTT.md)
//   /info     MQTT-style info JSON
// at http://192.168.4.1 on the hotspot, or the logger's address (logger-<device_id>.local) on a
// network.
//
// The page also sets the networks to join (and the master's address), then reboots to join:
//   /networks  GET: known network names; POST ssid, password[, mqtt_host] or clear=1
// and, while the master isn't connected, names a session and starts/pauses recording:
//   /recording POST action=start|stop[, session]  (refused while the master is in control)
//
// Config (wifi section): "enabled": true, "networks": [{ "ssid", "password" }],
//                        "hotspot": "no_master" | "no_network" | "always" | "never",
//                        "hotspot_password": "" (open; 8+ characters for WPA2)
#pragma once

#include <WebServer.h>

#include "feature.h"

namespace logger {

class WifiFeature : public Feature {
public:
    WifiFeature() : Feature("wifi", "wifi", 1) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void status(JsonObject out) const override;
    void poll() override;

    bool stationConnected() const;  // joined a network (the master's, for MQTT)
    const String& ssid() const { return ssid_; }

private:
    void tryNext();
    void setHotspot(bool on);
    void sendJson(bool info);
    void sendNetworks();
    void saveNetwork();
    void recordingAction();
    bool masterConnected() const;

    struct Network {
        String ssid, password;
    };
    static constexpr size_t MAX_NETWORKS = 4;

    Logger* host_ = nullptr;
    WebServer server_{80};
    Network networks_[MAX_NETWORKS];
    size_t nNetworks_ = 0;
    size_t current_ = 0;
    uint32_t attemptStarted_ = 0;
    bool wasConnected_ = false;
    const char* hotspotMode_ = "no_master";
    uint32_t masterSeen_ = 0;  // last time the master was connected (or boot)
    bool hotspotOn_ = false;
    String hotspotPassword_;
    String ssid_;      // hotspot name
    bool enabled_ = false;
    uint32_t rebootAt_ = 0;
};

}  // namespace logger
