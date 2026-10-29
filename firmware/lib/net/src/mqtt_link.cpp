#include "mqtt_link.h"

#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <logger.h>
#include <sys/time.h>

namespace logger {

namespace {

constexpr uint32_t BROKER_RETRY_MS = 30000;   // mDNS lookups, while no broker is found
constexpr uint32_t REDISCOVER_MS = 60000;     // disconnected this long: look the broker up again
volatile bool ntpSynced = false;              // set by the SNTP callback (another task)

void onNtpSync(struct timeval*) { ntpSynced = true; }

}  // namespace

void MqttLink::begin(Logger& host) {
    host_ = &host;
    JsonObjectConst core = host.config()["core"].as<JsonObjectConst>();
    configuredHost_ = core["mqtt_host"] | "";
    port_ = core["mqtt_port"] | 1883;
    user_ = core["mqtt_user"] | "";
    password_ = core["mqtt_password"] | "";
    ntpServer_ = core["ntp_server"] | "";
    statusIntervalMs_ = max(5, int(core["status_interval_s"] | 30)) * 1000UL;
    ntpIntervalMs_ = max(60, int(core["ntp_interval_s"] | 3600)) * 1000UL;
    for (JsonVariantConst g : core["groups"].as<JsonArrayConst>())
        if (nGroups_ < MAX_GROUPS && (g | "")[0]) groups_[nGroups_++] = g.as<const char*>();

    base_ = "loggers/" + host.deviceId() + "/";
    clientId_ = "logger-" + host.deviceId();
    willTopic_ = base_ + "status";
    willMessage_ = "{\"v\":1,\"state\":\"offline\"}";
    inbox_ = xQueueCreate(8, sizeof(String*));
}

// ------------------------------------------------------------------ connection

bool MqttLink::findBroker() {
    lastBrokerTry_ = millis();
    if (!configuredHost_.isEmpty()) {
        brokerHost_ = configuredHost_;
        return true;
    }
    // Blocks the main loop for up to ~3 s; recording runs in its own tasks and isn't affected
    const int n = MDNS.queryService("mqtt", "tcp");
    if (n <= 0) return false;
    brokerHost_ = MDNS.IP(0).toString();
    port_ = MDNS.port(0);
    return true;
}

void MqttLink::start() {
    uri_ = "mqtt://" + brokerHost_ + ":" + String(port_);
    esp_mqtt_client_config_t cfg = {};
    cfg.uri = uri_.c_str();
    cfg.client_id = clientId_.c_str();
    if (!user_.isEmpty()) {
        cfg.username = user_.c_str();
        cfg.password = password_.c_str();
    }
    cfg.lwt_topic = willTopic_.c_str();
    cfg.lwt_msg = willMessage_.c_str();
    cfg.lwt_msg_len = willMessage_.length();
    cfg.lwt_qos = 1;
    cfg.lwt_retain = 1;
    cfg.keepalive = 30;
    cfg.buffer_size = 4096;      // incoming: commands are small
    cfg.out_buffer_size = 8192;  // outgoing: info and publications
    client_ = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, onEvent, this);
    esp_mqtt_client_start(client_);
    disconnectedSince_ = millis();
}

void MqttLink::stop() {
    if (!client_) return;
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    connected_ = false;
}

// MQTT task: only flags and the inbox queue; everything else happens in poll()
void MqttLink::onEvent(void* self, esp_event_base_t, int32_t id, void* data) {
    MqttLink* link = static_cast<MqttLink*>(self);
    esp_mqtt_event_handle_t e = static_cast<esp_mqtt_event_handle_t>(data);
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            link->connected_ = true;
            link->justConnected_ = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            link->connected_ = false;
            break;
        case MQTT_EVENT_DATA: {
            if (e->total_data_len != e->data_len) break;  // fragmented: far bigger than any command
            String* msg = new String();
            msg->reserve(e->topic_len + 1 + e->data_len);
            msg->concat(e->topic, e->topic_len);
            msg->concat('\n');
            msg->concat(e->data, e->data_len);
            if (xQueueSend(link->inbox_, &msg, 0) != pdTRUE) delete msg;
            break;
        }
        default: break;
    }
}

void MqttLink::poll() {
    if (!host_) return;
    const uint32_t now = millis();

    if (!wifi_.stationConnected()) {
        if (client_ && connected_) disconnectedSince_ = now;
        return;  // the client reconnects by itself once WiFi is back
    }
    if (!client_) {
        if (now - lastBrokerTry_ < BROKER_RETRY_MS && lastBrokerTry_) return;
        if (findBroker()) start();
        return;
    }
    if (!connected_) {
        // Broker moved (new DHCP address)? Look it up again
        if (configuredHost_.isEmpty() && now - disconnectedSince_ > REDISCOVER_MS) stop();
        return;
    }
    disconnectedSince_ = now;

    if (justConnected_) {
        justConnected_ = false;
        onConnected();
    }
    String* msg;
    while (xQueueReceive(inbox_, &msg, 0) == pdTRUE) {
        const int nl = msg->indexOf('\n');
        handle(msg->substring(0, nl), msg->substring(nl + 1));
        delete msg;
    }
    if (announce_) {
        announce_ = false;
        publishInfo();
        publishConfig();
        publishStatus();
    }
    const char* state = host_->state();
    if (strcmp(state, lastState_) != 0 || now - lastStatus_ >= statusIntervalMs_ ||
        (statusSoon_ && now - lastStatus_ >= 2000)) {
        statusSoon_ = false;
        publishStatus();
    }
    if (ntpSynced) {
        ntpSynced = false;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        host_->setTime(int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000, "ntp");
        host_->event("time_synced", "core");
    }
}

void MqttLink::onConnected() {
    esp_mqtt_client_subscribe(client_, (base_ + "cmd").c_str(), 1);
    esp_mqtt_client_subscribe(client_, "loggers/all/cmd", 1);
    for (size_t i = 0; i < nGroups_; i++)
        esp_mqtt_client_subscribe(client_, ("loggers/group/" + groups_[i] + "/cmd").c_str(), 1);
    publishInfo();
    publishConfig();
    publishStatus();
    startNtp();
}

bool MqttLink::publish(const String& subtopic, const String& json, int qos, bool retain) {
    if (!client_ || !connected_) return false;
    // enqueue never blocks the caller; the MQTT task sends it
    return esp_mqtt_client_enqueue(client_, (base_ + subtopic).c_str(), json.c_str(), json.length(), qos,
                                   retain, true) >= 0;
}

// ------------------------------------------------------------------ messages

void MqttLink::publishInfo() {
    JsonDocument d;
    host_->buildInfo(d.to<JsonObject>());
    JsonObject n = d["network"].to<JsonObject>();
    n["interface"] = "wifi";
    n["ip"] = WiFi.localIP().toString();
    n["mac"] = WiFi.macAddress();
    String json;
    serializeJson(d, json);
    publish("info", json, 1, true);
}

void MqttLink::publishConfig() {
    JsonDocument d;
    d.set(host_->config());
    stripSecrets(d.as<JsonVariant>());
    d["v"] = 1;
    String json;
    serializeJson(d, json);
    publish("config", json, 1, true);
}

void MqttLink::publishStatus() {
    JsonDocument d;
    host_->buildStatus(d.to<JsonObject>());
    String json;
    serializeJson(d, json);
    if (publish("status", json, 1, true)) {
        lastStatus_ = millis();
        lastState_ = host_->state();  // a string literal, so it can be kept
    }
}

void MqttLink::handle(const String& topic, const String& payload) {
    JsonDocument in, ack;
    ack["v"] = 1;
    if (deserializeJson(in, payload) || !in.is<JsonObject>()) {
        ack["ok"] = false;
        ack["error"] = "invalid_args";
        ack["message"] = "not valid JSON";
    } else {
        ack["id"] = in["id"];
        String message;
        CmdStatus st = host_->dispatch(in["cmd"] | "", in["target"] | "", in["args"].as<JsonObjectConst>(),
                                       ack["result"].to<JsonObject>(), message);
        ack["ok"] = st == CmdStatus::Ok;
        if (st != CmdStatus::Ok) ack["error"] = cmdStatusCode(st);
        if (message.length()) ack["message"] = message;
    }
    String json;
    serializeJson(ack, json);
    publish("cmd/ack", json, 1, false);
    publishStatus();  // most commands change something worth seeing straight away
}

// ------------------------------------------------------------------ time

void MqttLink::startNtp() {
    const String& server = ntpServer_.isEmpty() ? brokerHost_ : ntpServer_;
    if (server.isEmpty()) return;
    // SNTP keeps a pointer to the name, so it lives in a fixed buffer
    if (ntpStarted_ && server == ntpHost_) {
        sntp_restart();
        return;
    }
    if (ntpStarted_) sntp_stop();
    ntpStarted_ = true;
    strlcpy(ntpHost_, server.c_str(), sizeof(ntpHost_));
    sntp_set_time_sync_notification_cb(onNtpSync);
    sntp_set_sync_interval(ntpIntervalMs_);
    configTime(0, 0, ntpHost_);
}

bool MqttLink::syncTime() {
    if (!connected_ || (ntpServer_.isEmpty() && brokerHost_.isEmpty())) return false;
    startNtp();
    return true;
}

}  // namespace logger
