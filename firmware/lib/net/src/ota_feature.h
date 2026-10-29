// The `ota` feature (protocol/features/ota.json): firmware updates over WiFi.
//
// From the master: the `update` command names a firmware image on the master's web server
// (path + port; the host is the one the MQTT broker was reached at) with its size and SHA-256.
// The image is streamed into the spare app slot (the running firmware is untouched), checked
// (size, SHA-256, ESP32 image, model marker), and the logger restarts into it. From the logger's
// own page: POST /update with the file (field use, no master).
//
// Rollback: the new firmware has confirm_timeout_s to prove itself — connected to the master
// (for master-sent updates) or simply running for a minute (page uploads). If it doesn't, or it
// restarts more than 3 times first, the logger switches back to the previous slot and restarts.
//
// Every image carries a marker "VLFW-MODEL=<model>;VLFW-VERSION=<version>;" (see the logger's
// main.cpp); an image for a different model, or with no marker, is refused.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "feature.h"

namespace logger {

class OtaFeature : public Feature {
public:
    // firmwareId: this image's "VLFW-MODEL=…;VLFW-VERSION=…;" marker (reported in info, which also
    // keeps the linker from dropping it)
    explicit OtaFeature(const char* firmwareId) : Feature("ota", "ota", 1), firmwareId_(firmwareId) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void describe(JsonObject info) const override { info["image"] = firmwareId_; }
    void status(JsonObject out) const override;
    bool hasCommand(const char* cmd) const override { return strcmp(cmd, "update") == 0; }
    CmdStatus command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) override;
    void poll() override;

    // Direct upload from the logger's page (WifiFeature): begin, write each chunk, end
    bool uploadBegin(String& error);
    bool uploadWrite(const uint8_t* data, size_t n);
    bool uploadEnd(String& error);
    void uploadAbort();
    bool busy() const;

private:
    enum class State : uint8_t { Idle, Downloading, Verifying, Rebooting, Confirming, Failed };

    void downloadTask();
    bool write(const uint8_t* data, size_t n);  // shared by download and upload: marker scan + flash
    void startWrite();
    bool finishWrite(String& error);
    void markPending(bool needMaster);
    void rollBack(const String& reason);
    void raise(const char* name, const String& detailJson);
    static const char* stateName(State s);

    const char* firmwareId_;
    Logger* host_ = nullptr;
    uint32_t confirmTimeoutS_ = 300;
    volatile State state_ = State::Idle;
    volatile uint8_t progress_ = 0;
    String version_;          // running
    String targetVersion_;    // being installed / confirmed
    String previousVersion_;  // before the last update
    String lastError_;
    String url_, sha256_;
    size_t size_ = 0;
    TaskHandle_t task_ = nullptr;

    // Marker scan across chunk boundaries
    String scanTail_;
    String foundModel_;
    String foundVersion_;

    // Confirming new firmware
    bool needMaster_ = false;
    uint32_t confirmUntil_ = 0;
    uint32_t rebootAt_ = 0;

    // Events from the download task or from before the master was connected, raised in poll()
    volatile bool taskDone_ = false;
    bool taskOk_ = false;
    String pendingEvent_, pendingDetail_;
};

}  // namespace logger
