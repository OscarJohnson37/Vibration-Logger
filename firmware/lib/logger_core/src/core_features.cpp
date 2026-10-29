#include "core_features.h"

#include "logger.h"

namespace logger {

namespace {
bool oneOf(const char* cmd, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (strcmp(cmd, n) == 0) return true;
    return false;
}
}  // namespace

// ------------------------------------------------------------------ core

bool CoreFeature::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    host.led().setBrightness(config["led_brightness"] | 0.2f);
    return true;
}

void CoreFeature::status(JsonObject out) const {
    out["temperature_c"] = temperatureRead();  // ESP32-S3 die temperature (approximate)
    JsonObject t = out["time"].to<JsonObject>();
    t["synced"] = host_->timeSynced();
    if (host_->timeSynced()) {
        t["source"] = host_->timeSource();
        t["last_sync_age_s"] = host_->lastSyncAgeS();
    }
    out["free_heap"] = ESP.getFreeHeap();
    out["reset_reason"] = host_->resetReason();
}

bool CoreFeature::hasCommand(const char* cmd) const {
    return oneOf(cmd, {"identify", "mark", "get_config", "set_config", "sync_time", "set_time", "reboot"});
}

CmdStatus CoreFeature::command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) {
    if (!strcmp(cmd, "identify")) {
        host_->led().identify(constrain(args["seconds"] | 10, 1, 300));
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "mark")) {
        const char* text = args["text"] | "";
        if (!*text) {
            message = "text is required";
            return CmdStatus::InvalidArgs;
        }
        if (!host_->recording()) {
            message = "not recording";
            return CmdStatus::Failed;
        }
        JsonDocument m;
        m["type"] = "note";
        m["text"] = text;
        m["source"] = "master";
        host_->marker(m);
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "get_config")) {
        result["config"].set(host_->config().as<JsonObjectConst>());
        stripSecrets(result["config"]);
        if (host_->net()) host_->net()->announce();
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "set_config")) return host_->setConfig(args["changes"].as<JsonObjectConst>(), result, message);
    if (!strcmp(cmd, "sync_time")) {
        if (host_->net() && host_->net()->syncTime()) return CmdStatus::Ok;
        message = "no time server (not connected to the master); use set_time";
        return CmdStatus::Failed;
    }
    if (!strcmp(cmd, "set_time")) {
        if (!args["utc_ms"].is<int64_t>()) {
            message = "utc_ms (integer) is required";
            return CmdStatus::InvalidArgs;
        }
        host_->setTime(args["utc_ms"].as<int64_t>(), "manual");
        host_->event("time_synced", "core");
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "reboot")) {
        rebootAt_ = millis() + 2000;  // give the ack (and the status after it) time to reach the master
        return CmdStatus::Ok;
    }
    return CmdStatus::Unsupported;
}

void CoreFeature::poll() {
    if (rebootAt_ && int32_t(millis() - rebootAt_) >= 0) {
        host_->stopRecording("reboot");
        ESP.restart();
    }
}

// ------------------------------------------------------------------ recording

bool RecordingFeature::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    autostart_ = config["autostart"] | true;
    autostartWithMaster_ = config["autostart_with_master"] | false;
    startDelayS_ = constrain(config["start_delay_s"] | 10, 0, 3600);
    flushIntervalS_ = constrain(config["flush_interval_s"] | 5, 1, 60);
    fileMaxMinutes_ = constrain(config["file_max_minutes"] | 60, 1, 1440);
    fileMaxMb_ = constrain(config["file_max_mb"] | 256, 1, 3900);
    if (host.resumeRecording()) {
        // It was recording when it stopped running: carry on, master or not, after the usual delay
        resuming_ = true;
        counting_ = true;
        countdownUntil_ = millis() + startDelayS_ * 1000;
        autostartState_ = "resuming";
    } else if (autostart_) {
        counting_ = true;
        countdownUntil_ = millis() + startDelayS_ * 1000;
        autostartState_ = "counting";
    }
    return true;
}

uint32_t RecordingFeature::countdownRemainingMs() const {
    int32_t left = int32_t(countdownUntil_ - millis());
    return counting_ && left > 0 ? uint32_t(left) : 0;
}

// Auto-start (PLAN.md, Recording control): after start_delay_s, unless the master is connected,
// in which case the master decides when to record. On the master's network but not yet talking to
// it, wait up to MASTER_WAIT_MS more, so a slow connection doesn't cause a stray recording.
void RecordingFeature::poll() {
    static constexpr uint32_t MASTER_WAIT_MS = 30000;
    uint32_t now = millis();
    Net* net = host_->net();
    if (counting_ && !resuming_ && !autostartWithMaster_ && net && net->connected()) {
        counting_ = false;
        autostartState_ = "skipped_master";
        host_->event("autostart_skipped", instance());
    } else if (counting_ && int32_t(now - countdownUntil_) >= 0) {
        if (!resuming_ && !autostartWithMaster_ && net && net->linkUp()) {
            if (!masterWaitUntil_) masterWaitUntil_ = now + MASTER_WAIT_MS;
            if (int32_t(now - masterWaitUntil_) < 0) {
                autostartState_ = "waiting_for_master";
                return;
            }
        }
        counting_ = false;
        String message;
        if (host_->startRecording(resuming_ ? "resume" : "autostart", message)) {
            autostartState_ = resuming_ ? "resumed" : "started";
        } else {
            counting_ = true;  // keep retrying, e.g. until a card is inserted
            countdownUntil_ = now + 5000;
            autostartState_ = "counting";
        }
    }
    if (!host_->recording()) return;
    if (now - lastFlush_ >= flushIntervalS_ * 1000) {
        lastFlush_ = now;
        host_->flushSinks();
    }
    if (now - host_->fileOpenedMs() >= fileMaxMinutes_ * 60000UL ||
        host_->fileBytes() >= size_t(fileMaxMb_) * 1024 * 1024)
        host_->rollFile();
}

void RecordingFeature::status(JsonObject out) const {
    out["recording"] = host_->recording();
    out["session"] = host_->session();
    out["autostart"] = autostartState_;
    if (counting_) out["countdown_s"] = (countdownRemainingMs() + 999) / 1000;
    if (host_->recording()) {
        out["file"] = host_->fileName();
        int64_t started = host_->recordingStartedUtcMs();
        if (started >= 0) out["started_ms"] = started;
        else out["started_ms"] = nullptr;
    }
    out["files_this_boot"] = host_->filesThisBoot();
    out["bytes_written"] = host_->bytesWritten();
    out["dropped_blocks"] = host_->droppedBlocks();
    out["queue_high_water"] = host_->queueHighWater();
}

bool RecordingFeature::hasCommand(const char* cmd) const {
    return oneOf(cmd, {"start", "stop", "roll_file"});
}

CmdStatus RecordingFeature::command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) {
    if (!strcmp(cmd, "start")) {
        if (counting_) autostartState_ = "off";
        counting_ = false;
        const char* session = args["session"] | "";
        if (*session) {
            // Stored, so a reboot or auto-start carries on in the same session
            JsonDocument changes;
            changes["recording"]["session"] = safeName(session);
            JsonDocument ignored;
            CmdStatus st = host_->setConfig(changes.as<JsonObjectConst>(), ignored.to<JsonObject>(), message);
            if (st != CmdStatus::Ok) return st;
            if (host_->recording()) {
                host_->rollFile();  // carry on in the new session
                result["session"] = host_->session();
                return CmdStatus::Ok;
            }
        }
        if (!host_->startRecording("command", message)) return CmdStatus::Failed;
        result["session"] = host_->session();
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "stop")) {
        if (counting_) autostartState_ = "off";
        counting_ = false;
        host_->stopRecording("command");
        return CmdStatus::Ok;
    }
    if (!strcmp(cmd, "roll_file")) {
        if (!host_->recording()) {
            message = "not recording";
            return CmdStatus::Failed;
        }
        host_->rollFile();
        return CmdStatus::Ok;
    }
    return CmdStatus::Unsupported;
}

}  // namespace logger
