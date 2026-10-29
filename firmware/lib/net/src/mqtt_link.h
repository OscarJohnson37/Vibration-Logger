// The logger's MQTT link to the master (protocol/MQTT.md), over the wifi feature's network.
//
// Once WiFi has joined a network: finds the broker (core.mqtt_host, else mDNS _mqtt._tcp),
// connects as logger-<device_id> with a retained "offline" last will, publishes retained info,
// config and status, then status every core.status_interval_s and whenever the state changes.
// Commands from loggers/<id>/cmd, loggers/group/<group>/cmd and loggers/all/cmd go through the
// Logger's dispatch, and each gets exactly one ack. The clock is synced by NTP from
// core.ntp_server (default: the broker's host) on connect and every core.ntp_interval_s.
//
// Recording never depends on any of this: with no network, nothing here does anything.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>

#include <net.h>

#include "wifi_feature.h"

namespace logger {

class MqttLink : public Net {
public:
    explicit MqttLink(WifiFeature& wifi) : wifi_(wifi) {}

    void begin(Logger& host) override;
    void poll() override;
    bool connected() const override { return connected_; }
    bool linkUp() const override { return wifi_.stationConnected(); }
    bool publish(const String& subtopic, const String& json, int qos = 1, bool retain = false) override;
    void announce() override { announce_ = true; }
    void statusSoon() override { statusSoon_ = true; }
    bool syncTime() override;
    String masterHost() const override { return connected_ ? brokerHost_ : String(); }

private:
    static void onEvent(void* self, esp_event_base_t, int32_t id, void* data);
    bool findBroker();
    void start();
    void stop();
    void onConnected();
    void handle(const String& topic, const String& payload);
    void publishInfo();
    void publishConfig();
    void publishStatus();
    void startNtp();

    Logger* host_ = nullptr;
    WifiFeature& wifi_;
    esp_mqtt_client_handle_t client_ = nullptr;
    QueueHandle_t inbox_ = nullptr;  // String* "topic\npayload", from the MQTT task
    volatile bool connected_ = false;
    volatile bool justConnected_ = false;
    bool announce_ = false;
    bool statusSoon_ = false;

    // From core config at begin
    String configuredHost_, user_, password_, ntpServer_;
    uint16_t port_ = 1883;
    uint32_t statusIntervalMs_ = 30000;
    uint32_t ntpIntervalMs_ = 3600000;
    static constexpr size_t MAX_GROUPS = 4;
    String groups_[MAX_GROUPS];
    size_t nGroups_ = 0;

    String base_;         // loggers/<device_id>/
    String brokerHost_;   // resolved host or IP
    String uri_, clientId_, willTopic_, willMessage_;
    uint32_t lastBrokerTry_ = 0;
    uint32_t disconnectedSince_ = 0;
    uint32_t lastStatus_ = 0;
    const char* lastState_ = "";
    bool ntpStarted_ = false;
    char ntpHost_[64] = "";
};

}  // namespace logger
