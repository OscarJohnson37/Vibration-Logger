#include "logger.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <vector>

namespace logger {

const char* cmdStatusCode(CmdStatus s) {
    switch (s) {
        case CmdStatus::Ok: return "ok";
        case CmdStatus::Unsupported: return "unsupported";
        case CmdStatus::UnknownTarget: return "unknown_target";
        case CmdStatus::AmbiguousTarget: return "ambiguous_target";
        case CmdStatus::InvalidArgs: return "invalid_args";
        case CmdStatus::Busy: return "busy";
        case CmdStatus::Failed: return "failed";
    }
    return "failed";
}

void stripSecrets(JsonVariant v) {
    if (v.is<JsonObject>()) {
        JsonObject o = v.as<JsonObject>();
        std::vector<String> secret;
        for (JsonPair kv : o) {
            if (strstr(kv.key().c_str(), "password")) secret.push_back(kv.key().c_str());
            else stripSecrets(kv.value());
        }
        for (const String& k : secret) o.remove(k);
    } else if (v.is<JsonArray>()) {
        for (JsonVariant e : v.as<JsonArray>()) stripSecrets(e);
    }
}

namespace {

constexpr size_t QUEUE_DEPTH = 512;   // items; sample data itself lives in PSRAM
constexpr size_t PSRAM_RESERVE = 512 * 1024;  // stop buffering before PSRAM is exhausted

const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_EXT: return "external";
        default: return "unknown";
    }
}

void* bufferAlloc(size_t n) {
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > PSRAM_RESERVE + n)
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------------ setup

void Logger::add(Feature& f) {
    if (nFeatures_ < MAX_FEATURES) features_[nFeatures_++] = &f;
}

void Logger::addSink(Sink& s) {
    if (nSinks_ < MAX_SINKS) sinks_[nSinks_++] = &s;
}

void Logger::begin(const char* defaultConfigJson) {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(5);  // never 0: HWCDC write() hangs forever once the host closes the port

    // Identity: MAC-based ID, boot counter and serial number in NVS (never on removable storage)
    uint64_t mac = ESP.getEfuseMac();
    char id[13];
    for (int i = 0; i < 6; i++) snprintf(id + 2 * i, 3, "%02x", uint8_t(mac >> (8 * i)));
    deviceId_ = id;
    Preferences prefs;
    prefs.begin("logger", false);
    bootId_ = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", bootId_);
    serial_ = prefs.getString("serial", "");
    prefs.end();
    resetReason_ = resetReasonName(esp_reset_reason());

    // Config: the logger type's compiled-in defaults for now. Phase 3 layers the stored config
    // from internal flash on top (and config.json from an SD card, on loggers that have one).
    if (deserializeJson(config_, defaultConfigJson ? defaultConfigJson : "{}") || !config_.is<JsonObject>()) {
        config_.clear();
        config_.to<JsonObject>();
    }

    // Built-in features first, then the logger's own
    Feature* user[MAX_FEATURES];
    size_t nUser = nFeatures_;
    memcpy(user, features_, sizeof(Feature*) * nUser);
    nFeatures_ = 0;
    add(coreFeature_);
    add(recordingFeature_);
    for (size_t i = 0; i < nUser; i++) add(*user[i]);

    for (size_t i = 0; i < nFeatures_; i++) {
        Feature* f = features_[i];
        for (size_t s = 0; s < f->streamCount(); s++) f->stream(s)->id = nextStreamId_++;
    }

    led_.begin(config_["core"]["led_brightness"] | 0.2f);

    // Sinks enabled by recording.sinks. Default: every sink this logger has. Nothing assumes an
    // SD card: an Ethernet logger may have only a network sink.
    JsonArrayConst wanted = config_["recording"]["sinks"].as<JsonArrayConst>();
    for (size_t i = 0; i < nSinks_; i++) {
        sinkEnabled_[i] = wanted.isNull();
        for (JsonVariantConst w : wanted)
            if (strcmp(w | "", sinks_[i]->name()) == 0) sinkEnabled_[i] = true;
    }

    queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(Item));
    xTaskCreatePinnedToCore([](void* self) { static_cast<Logger*>(self)->writerTask(); },
                            "writer", 8192, this, 3, nullptr, 0);

    for (size_t i = 0; i < nFeatures_; i++)
        features_[i]->begin(*this, config_[features_[i]->instance()].as<JsonObjectConst>());

    xTaskCreatePinnedToCore([](void* self) { static_cast<Logger*>(self)->acquisitionTask(); },
                            "acquire", 8192, this, 10, nullptr, 1);
}

void Logger::loop() {
    handleConsole();
    for (size_t i = 0; i < nFeatures_; i++) features_[i]->poll();
    for (size_t i = 0; i < nSinks_; i++) {
        if (!sinkEnabled_[i]) continue;
        sinks_[i]->poll();
        if (sinks_[i]->wantsRestart() && recording_) {
            // Reopen just this sink with a fresh file (new index, so SD names never collide)
            closeSinks(1 << i);
            fileIndex_++;
            openSinks(1 << i);
        }
    }
    updateLed();
    led_.update();
    delay(1);
}

void Logger::acquisitionTask() {
    for (;;) {
        for (size_t i = 0; i < nFeatures_; i++) features_[i]->acquire();
        vTaskDelay(1);
    }
}

bool Logger::setSerial(const String& serial) {
    if (serial.isEmpty() || serial.length() > 32) return false;
    Preferences prefs;
    prefs.begin("logger", false);
    prefs.putString("serial", serial);
    prefs.end();
    serial_ = serial;
    return true;
}

// ------------------------------------------------------------------ writer queue

bool Logger::enqueue(Item& it) {
    if (!queue_ || xQueueSend(queue_, &it, 0) != pdTRUE) {
        if (it.values) heap_caps_free(it.values);
        delete it.text;
        return false;
    }
    uint32_t used = QUEUE_DEPTH - uxQueueSpacesAvailable(queue_);
    if (used > queueHighWater_) queueHighWater_ = used;
    return true;
}

void Logger::enqueueText(Item::Kind kind, uint8_t mask, const String& text, uint64_t t, uint32_t fileIndex) {
    Item it = {};
    it.kind = kind;
    it.sinkMask = mask;
    it.t = t;
    it.fileIndex = fileIndex;
    it.text = new String(text);
    enqueue(it);
}

void Logger::writerTask() {
    Item it;
    for (;;) {
        if (xQueueReceive(queue_, &it, portMAX_DELAY) != pdTRUE) continue;
        if (it.kind == Item::Text) {
            Serial.print(*it.text);
        } else {
            for (size_t i = 0; i < nSinks_; i++) {
                if (!sinkEnabled_[i] || !(it.sinkMask & (1 << i))) continue;
                Sink* s = sinks_[i];
                switch (it.kind) {
                    case Item::Open:
                        if (sinkOpen_[i]) s->close();
                        sinkOpen_[i] = s->available() && s->open(*it.text, deviceId_, bootId_, it.fileIndex);
                        break;
                    case Item::Close:
                        if (sinkOpen_[i]) s->close();
                        sinkOpen_[i] = false;
                        break;
                    case Item::Flush:
                        if (sinkOpen_[i]) s->flush();
                        break;
                    case Item::Marker:
                        if (sinkOpen_[i]) s->marker(it.t, *it.text);
                        break;
                    case Item::Data: {
                        if (!sinkOpen_[i]) break;
                        SampleBlock b;
                        b.stream = it.stream;
                        b.flags = it.flags;
                        b.nFrames = it.nFrames;
                        b.firstFrame = it.firstFrame;
                        b.tMonoUs = it.t;
                        if (it.stream->dtype == Dtype::F32) b.f32 = static_cast<const float*>(it.values);
                        else b.i32 = static_cast<const int32_t*>(it.values);
                        s->write(b);
                        break;
                    }
                    case Item::Text: break;
                }
            }
        }
        if (it.values) heap_caps_free(it.values);
        delete it.text;
    }
}

void Logger::print(const String& text) {
    enqueueText(Item::Text, 0, text);
}

// ------------------------------------------------------------------ time

uint64_t Logger::monoUs() const {
    return uint64_t(esp_timer_get_time());
}

int64_t Logger::utcMs() const {
    return timeSynced_ ? (int64_t(monoUs()) + utcOffsetUs_) / 1000 : -1;
}

void Logger::setTime(int64_t utcMs, const char* source) {
    uint64_t mono = monoUs();
    utcOffsetUs_ = utcMs * 1000 - int64_t(mono);
    timeSynced_ = true;
    timeSource_ = source;
    lastSyncMs_ = millis();
    if (recording_) {
        JsonDocument m;
        m["type"] = "time_sync";
        m["utc_us"] = utcMs * 1000;
        m["source"] = source;
        m["uncertainty_us"] = strcmp(source, "manual") == 0 ? 1000000 : 10000;
        String json;
        serializeJson(m, json);
        enqueueText(Item::Marker, ALL_SINKS, json, mono);
    }
}

// ------------------------------------------------------------------ recording mechanics

bool Logger::anySinkAvailable() {
    for (size_t i = 0; i < nSinks_; i++)
        if (sinkEnabled_[i] && sinks_[i]->available()) return true;
    return false;
}

bool Logger::startRecording(const char* reason, String& message) {
    if (recording_) return true;
    if (!anySinkAvailable()) {
        message = "no recording sink available (recording.sinks; SD card inserted?)";
        return false;
    }
    recordingStartedUtcMs_ = utcMs();
    openSinks(ALL_SINKS);
    recording_ = true;
    JsonDocument m;
    m["type"] = "recording";
    m["state"] = "start";
    m["reason"] = reason;
    marker(m);
    for (size_t i = 0; i < nFeatures_; i++) features_[i]->startAcquisition();
    return true;
}

void Logger::stopRecording(const char* reason) {
    if (!recording_) return;
    for (size_t i = 0; i < nFeatures_; i++) features_[i]->stopAcquisition();
    JsonDocument m;
    m["type"] = "recording";
    m["state"] = "stop";
    m["reason"] = reason;
    marker(m);
    recording_ = false;
    closeSinks(ALL_SINKS);
    fileIndex_++;
}

void Logger::rollFile() {
    if (!recording_) return;
    closeSinks(ALL_SINKS);
    fileIndex_++;
    openSinks(ALL_SINKS);
}

void Logger::openSinks(uint8_t mask) {
    fileOpenedMs_ = millis();
    filesThisBoot_++;
    enqueueText(Item::Open, mask, buildMetadata(), 0, fileIndex_);
}

void Logger::closeSinks(uint8_t mask) {
    Item it = {};
    it.kind = Item::Close;
    it.sinkMask = mask;
    enqueue(it);
}

void Logger::flushSinks() {
    Item it = {};
    it.kind = Item::Flush;
    it.sinkMask = ALL_SINKS;
    enqueue(it);
}

size_t Logger::bytesWritten() const {
    size_t n = 0;
    for (size_t i = 0; i < nSinks_; i++)
        if (sinkEnabled_[i]) n = max(n, sinks_[i]->bytesWritten());
    return n;
}

size_t Logger::fileBytes() const {
    size_t n = 0;
    for (size_t i = 0; i < nSinks_; i++)
        if (sinkEnabled_[i]) n = max(n, sinks_[i]->fileBytes());
    return n;
}

String Logger::fileName() const {
    char name[32];
    snprintf(name, sizeof(name), "b%06u_f%04u.dlog", unsigned(bootId_), unsigned(fileIndex_));
    return name;
}

// ------------------------------------------------------------------ services for features

void Logger::submit(const SampleBlock& b) {
    if (!recording_ || !b.stream || !b.nFrames) return;
    const size_t bytes = size_t(b.nFrames) * b.stream->nChannels * 4;  // int32 or float
    Item it = {};
    it.kind = Item::Data;
    it.sinkMask = ALL_SINKS;
    it.flags = b.flags;
    it.stream = b.stream;
    it.nFrames = b.nFrames;
    it.firstFrame = b.firstFrame;
    it.t = b.tMonoUs;
    it.values = bufferAlloc(bytes);
    if (!it.values) {
        droppedBlocks_++;  // the reader sees the gap from the frame counter
        return;
    }
    memcpy(it.values, b.stream->dtype == Dtype::F32 ? static_cast<const void*>(b.f32) : static_cast<const void*>(b.i32), bytes);
    if (!enqueue(it)) droppedBlocks_++;
}

void Logger::marker(JsonDocument& body) {
    if (!recording_) return;
    String json;
    serializeJson(body, json);
    enqueueText(Item::Marker, ALL_SINKS, json, monoUs());
}

void Logger::event(const char* name, const char* source, JsonObjectConst detail) {
    JsonDocument m;
    m["type"] = "event";
    m["name"] = name;
    m["source"] = source;
    if (!detail.isNull()) m["detail"] = detail;
    marker(m);
    // phase 5: also publish on loggers/<id>/events
}

// ------------------------------------------------------------------ metadata, info, status

namespace {

void writeStream(Feature& f, size_t i, JsonObject s) {
    const StreamInfo& st = *f.stream(i);
    s["id"] = st.id;
    s["name"] = st.name;
    s["feature"] = st.feature;
    s["rate_hz"] = st.rateHz;
    s["dtype"] = dtypeName(st.dtype);
    if (st.hasRawRange) {
        s["raw_min"] = st.rawMin;
        s["raw_max"] = st.rawMax;
    }
    JsonArray ch = s["channels"].to<JsonArray>();
    for (size_t c = 0; c < st.nChannels; c++) {
        JsonObject o = ch.add<JsonObject>();
        o["name"] = st.channels[c].name;
        o["unit"] = st.channels[c].unit;
        o["scale"] = st.channels[c].scale;
        o["offset"] = st.channels[c].offset;
    }
    f.streamMetadata(i, s);
}

}  // namespace

String Logger::buildMetadata() {
    JsonDocument d;
    d["format"] = "dlog";
    d["format_version"] = "1.0";
    JsonObject dev = d["device"].to<JsonObject>();
    dev["id"] = deviceId_;
    if (!serial_.isEmpty()) dev["serial"] = serial_;
    const char* name = config_["core"]["name"] | "";
    dev["name"] = *name ? name : serial_.c_str();
    dev["type"] = type_;
    dev["hardware"] = hardware_;
    dev["firmware"] = firmware_;
    d["boot"]["id"] = bootId_;
    d["boot"]["reason"] = resetReason_;
    uint64_t mono = monoUs();
    d["file"]["index"] = fileIndex_;
    d["file"]["opened_mono_us"] = mono;
    JsonObject t = d["time"].to<JsonObject>();
    t["synced"] = timeSynced_;
    t["mono_us"] = mono;
    if (timeSynced_) {
        t["utc_us"] = int64_t(mono) + utcOffsetUs_;
        t["source"] = timeSource_;
    }
    JsonArray streams = d["streams"].to<JsonArray>();
    for (size_t i = 0; i < nFeatures_; i++)
        for (size_t s = 0; s < features_[i]->streamCount(); s++) writeStream(*features_[i], s, streams.add<JsonObject>());
    d["config"].set(config_.as<JsonObjectConst>());
    stripSecrets(d["config"]);
    String out;
    serializeJson(d, out);
    return out;
}

void Logger::buildInfo(JsonObject out) {
    out["v"] = 1;
    JsonObject dev = out["device"].to<JsonObject>();
    dev["id"] = deviceId_;
    dev["serial"] = serial_;
    const char* name = config_["core"]["name"] | "";
    dev["name"] = *name ? name : serial_.c_str();
    dev["type"] = type_;
    dev["hardware"] = hardware_;
    dev["firmware"] = firmware_;
    JsonArray feats = out["features"].to<JsonArray>();
    for (size_t i = 0; i < nFeatures_; i++) {
        JsonObject f = feats.add<JsonObject>();
        f["instance"] = features_[i]->instance();
        f["feature"] = features_[i]->feature();
        f["version"] = features_[i]->version();
        features_[i]->describe(f);
    }
    JsonArray streams = out["streams"].to<JsonArray>();
    for (size_t i = 0; i < nFeatures_; i++)
        for (size_t s = 0; s < features_[i]->streamCount(); s++) writeStream(*features_[i], s, streams.add<JsonObject>());
    out["groups"].set(config_["core"]["groups"].as<JsonArrayConst>());
}

void Logger::buildStatus(JsonObject out) {
    out["v"] = 1;
    int64_t utc = utcMs();
    if (utc >= 0) out["t"] = utc;
    else out["t"] = nullptr;
    out["uptime_ms"] = millis();
    bool allOk = true;
    for (size_t i = 0; i < nFeatures_; i++) allOk &= features_[i]->ok();
    out["state"] = !allOk ? "error"
                   : recording_ ? "recording"
                   : recordingFeature_.countingDown() ? "starting"
                   : "idle";
    out["boot_id"] = bootId_;
    out["errors"].to<JsonArray>();
    JsonObject feats = out["features"].to<JsonObject>();
    for (size_t i = 0; i < nFeatures_; i++) features_[i]->status(feats[features_[i]->instance()].to<JsonObject>());
}

// ------------------------------------------------------------------ commands

CmdStatus Logger::dispatch(const char* cmd, const char* target, JsonObjectConst args, JsonObject result, String& message) {
    if (target && *target) {
        for (size_t i = 0; i < nFeatures_; i++) {
            if (strcmp(features_[i]->instance(), target) != 0) continue;
            if (!features_[i]->hasCommand(cmd)) {
                message = String("'") + target + "' has no command '" + cmd + "'";
                return CmdStatus::Unsupported;
            }
            return features_[i]->command(cmd, args, result, message);
        }
        message = String("no feature instance '") + target + "'";
        return CmdStatus::UnknownTarget;
    }
    Feature* found = nullptr;
    for (size_t i = 0; i < nFeatures_; i++) {
        if (!features_[i]->hasCommand(cmd)) continue;
        if (found) {
            message = String("more than one instance provides '") + cmd + "'; set target";
            return CmdStatus::AmbiguousTarget;
        }
        found = features_[i];
    }
    if (!found) {
        message = String("no feature provides '") + cmd + "'";
        return CmdStatus::Unsupported;
    }
    return found->command(cmd, args, result, message);
}

// ------------------------------------------------------------------ LED

void Logger::updateLed() {
    bool allOk = true;
    for (size_t i = 0; i < nFeatures_; i++) allOk &= features_[i]->ok();

    if (!allOk || !anySinkAvailable()) {
        led_.set(LedPattern::Error);
    } else if (recordingFeature_.countingDown()) {
        led_.set(LedPattern::Countdown);
        led_.setCountdown(recordingFeature_.countdownRemainingMs());
    } else if (recording_) {
        led_.set(LedPattern::Recording);
    } else {
        led_.set(LedPattern::Idle);
    }
}

// ------------------------------------------------------------------ USB console (bench)
//
//   dlog              restart the USB DLOG stream with fresh metadata (used by capture)
//   serial VL-0001    store this unit's serial number (once, when it's built)
//   info / status     print MQTT-style info/status JSON, prefixed with '#'
//   {"cmd": ...}      run a command exactly as MQTT would; prints the ack, prefixed with '#'
// Output goes through the writer queue, so it never lands inside a binary DLOG record.

void Logger::handleConsole() {
    while (Serial.available()) {
        char c = char(Serial.read());
        if (c == '\n' || c == '\r') {
            if (consoleBuf_.length()) consoleLine(consoleBuf_);
            consoleBuf_ = "";
        } else if (consoleBuf_.length() < 512) {
            consoleBuf_ += c;
        }
    }
}

void Logger::consoleLine(String line) {
    line.trim();
    String out;
    if (line == "dlog") {
        for (size_t i = 0; i < nSinks_; i++)
            if (strcmp(sinks_[i]->name(), "usb") == 0) sinks_[i]->requestRestart();
        return;
    } else if (line.startsWith("serial ")) {
        String s = line.substring(7);
        s.trim();
        out = String("#serial ") + (setSerial(s) ? serial_ : String("invalid (1-32 characters)")) + "\n";
    } else if (line == "info" || line == "status") {
        JsonDocument d;
        line == "info" ? buildInfo(d.to<JsonObject>()) : buildStatus(d.to<JsonObject>());
        out = "#" + line + " ";
        serializeJson(d, out);
        out += "\n";
    } else if (line.startsWith("{")) {
        JsonDocument in, ack;
        ack["v"] = 1;
        if (deserializeJson(in, line)) {
            ack["ok"] = false;
            ack["error"] = "invalid_args";
            ack["message"] = "not valid JSON";
        } else {
            ack["id"] = in["id"];
            String message;
            CmdStatus st = dispatch(in["cmd"] | "", in["target"] | "", in["args"].as<JsonObjectConst>(),
                                    ack["result"].to<JsonObject>(), message);
            ack["ok"] = st == CmdStatus::Ok;
            if (st != CmdStatus::Ok) ack["error"] = cmdStatusCode(st);
            if (message.length()) ack["message"] = message;
        }
        out = "#ack ";
        serializeJson(ack, out);
        out += "\n";
    } else {
        return;
    }
    print(out);
}

}  // namespace logger
