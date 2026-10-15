// The feature host at the centre of every logger's firmware (docs/PLAN.md, Feature model).
//
//   logger::Logger app("vibration", "TinyS3[D]", "0.3.0");
//   logger::Adxl355 accel("accel", pins…);
//   logger::UsbSink usb;
//   void setup() { app.add(accel); app.addSink(usb); app.begin(DEFAULT_CONFIG_JSON); }
//   void loop()  { app.loop(); }
//
// The host owns identity, config, time, the status LED, recording mechanics (files/sinks) and
// command routing. Behaviour lives in features, including the built-in core and recording.
//
// Three tasks keep slow storage from ever costing samples:
//   acquisition (core 1, high priority): calls Feature::acquire() every ~1 ms; sensors submit()
//   main loop   (core 1):                console, Feature::poll(), recording policy, LED
//   writer      (core 0):                drains a queue to the sinks; the only code touching them
// Sample blocks are copied into PSRAM on submit(), so a sink can stall for seconds (SD card
// housekeeping) without the sensor FIFO overflowing.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "core_features.h"
#include "feature.h"
#include "sink.h"
#include "status_led.h"

namespace logger {

class Logger {
public:
    Logger(const char* type, const char* hardware, const char* firmware)
        : type_(type), hardware_(hardware), firmware_(firmware) {}

    // Setup
    void add(Feature& f);
    void addSink(Sink& s);
    void begin(const char* defaultConfigJson);
    void loop();

    // Identity
    const String& deviceId() const { return deviceId_; }
    const String& serial() const { return serial_; }
    uint32_t bootId() const { return bootId_; }
    bool setSerial(const String& serial);

    // Time: device monotonic clock, plus a UTC mapping once set/synced
    uint64_t monoUs() const;
    bool timeSynced() const { return timeSynced_; }
    int64_t utcMs() const;  // -1 if not synced
    void setTime(int64_t utcMs, const char* source);

    // Recording mechanics (policy lives in RecordingFeature). Main loop only.
    bool startRecording(const char* reason, String& message);
    void stopRecording(const char* reason);
    void rollFile();
    void flushSinks();
    bool recording() const { return recording_; }
    bool anySinkAvailable();
    uint32_t fileIndex() const { return fileIndex_; }
    uint32_t filesThisBoot() const { return filesThisBoot_; }
    uint32_t fileOpenedMs() const { return fileOpenedMs_; }
    int64_t recordingStartedUtcMs() const { return recordingStartedUtcMs_; }
    size_t bytesWritten() const;
    size_t fileBytes() const;
    String fileName() const;
    uint32_t droppedBlocks() const { return droppedBlocks_; }
    uint32_t queueHighWater() const { return queueHighWater_; }

    // Services for features
    void submit(const SampleBlock& b);       // sensor data -> sinks (any task)
    void marker(JsonDocument& body);         // DLOG marker, if recording (main loop)
    void event(const char* name, const char* source, JsonObjectConst detail = JsonObjectConst());
    void print(const String& text);          // console output, ordered with the USB data stream
    StatusLed& led() { return led_; }
    RecordingFeature& recordingFeature() { return recordingFeature_; }

    // Commands, info, status, config (MQTT.md shapes)
    CmdStatus dispatch(const char* cmd, const char* target, JsonObjectConst args, JsonObject result, String& message);
    void buildInfo(JsonObject out);
    void buildStatus(JsonObject out);
    const JsonDocument& config() const { return config_; }

private:
    struct Item {
        enum Kind : uint8_t { Data, Marker, Open, Close, Flush, Text } kind;
        uint8_t sinkMask;
        uint16_t flags;
        const StreamInfo* stream;
        uint32_t nFrames;
        uint32_t fileIndex;
        uint64_t firstFrame;
        uint64_t t;       // t_mono_us for data/markers
        void* values;     // PSRAM copy of the samples (Data)
        String* text;     // JSON / metadata / console text
    };
    static constexpr uint8_t ALL_SINKS = 0xFF;

    bool enqueue(Item& it);
    void enqueueText(Item::Kind kind, uint8_t mask, const String& text, uint64_t t = 0, uint32_t fileIndex = 0);
    void openSinks(uint8_t mask);
    void closeSinks(uint8_t mask);
    void writerTask();
    void acquisitionTask();
    String buildMetadata();
    void updateLed();
    void handleConsole();
    void consoleLine(String line);

    const char* type_;
    const char* hardware_;
    const char* firmware_;
    String deviceId_, serial_, resetReason_;
    uint32_t bootId_ = 0;

    JsonDocument config_;
    static constexpr size_t MAX_FEATURES = 16, MAX_SINKS = 4;
    Feature* features_[MAX_FEATURES] = {};
    size_t nFeatures_ = 0;
    Sink* sinks_[MAX_SINKS] = {};
    bool sinkEnabled_[MAX_SINKS] = {};
    bool sinkOpen_[MAX_SINKS] = {};  // writer task only
    size_t nSinks_ = 0;
    uint16_t nextStreamId_ = 1;

    CoreFeature coreFeature_;
    RecordingFeature recordingFeature_;
    StatusLed led_;

    QueueHandle_t queue_ = nullptr;
    volatile uint32_t droppedBlocks_ = 0;
    volatile uint32_t queueHighWater_ = 0;

    volatile bool recording_ = false;
    uint32_t fileIndex_ = 0;
    uint32_t filesThisBoot_ = 0;
    uint32_t fileOpenedMs_ = 0;
    int64_t recordingStartedUtcMs_ = -1;

    bool timeSynced_ = false;
    int64_t utcOffsetUs_ = 0;  // utc_us = mono_us + offset
    const char* timeSource_ = "";
    uint32_t lastSyncMs_ = 0;

    String consoleBuf_;
};

}  // namespace logger
