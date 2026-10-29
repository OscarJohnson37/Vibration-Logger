#include "ota_feature.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <logger.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include <vector>

namespace logger {

namespace {

constexpr int MAX_BOOT_ATTEMPTS = 3;           // restarts of new firmware before giving up on it
constexpr uint32_t PAGE_CONFIRM_MS = 60000;    // page uploads: running this long = healthy
constexpr uint32_t STALL_TIMEOUT_MS = 15000;   // no bytes for this long: download failed
const char MARKER[] = "VLFW-MODEL=";
const size_t MARKER_TAIL = 96;                 // bytes kept between chunks for the marker scan

// Value after "<key>" up to ';' in raw bytes, or empty
String fieldAfter(const uint8_t* data, size_t n, const char* key) {
    const size_t k = strlen(key);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(data + i, key, k) != 0) continue;
        String v;
        for (size_t j = i + k; j < n && data[j] != ';' && v.length() < 40; j++) v += char(data[j]);
        return v;
    }
    return String();
}

}  // namespace

const char* OtaFeature::stateName(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::Downloading: return "downloading";
        case State::Verifying: return "verifying";
        case State::Rebooting: return "rebooting";
        case State::Confirming: return "confirming";
        case State::Failed: return "failed";
    }
    return "idle";
}

// ------------------------------------------------------------------ boot: confirm or roll back

bool OtaFeature::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    confirmTimeoutS_ = max(60, int(config["confirm_timeout_s"] | 300));
    version_ = host.firmware();

    Preferences p;
    p.begin("ota", false);
    previousVersion_ = p.getString("prevver", "");
    lastError_ = p.getString("lasterr", "");
    pendingEvent_ = p.getString("evt", "");      // left by a rollback for this firmware to report
    pendingDetail_ = p.getString("evtdetail", "");
    if (!pendingEvent_.isEmpty()) {
        p.remove("evt");
        p.remove("evtdetail");
    }
    if (p.getBool("pending", false)) {
        const String prev = p.getString("prev", "");
        targetVersion_ = p.getString("target", "");
        needMaster_ = p.getBool("needmaster", true);
        if (prev == esp_ota_get_running_partition()->label) {
            // The bootloader didn't accept the new image and started the old one
            p.putBool("pending", false);
            lastError_ = "new firmware " + targetVersion_ + " didn't start; still on " + version_;
            p.putString("lasterr", lastError_);
            pendingEvent_ = "update_rolled_back";
            pendingDetail_ = "{\"version\":\"" + targetVersion_ + "\",\"previous_version\":\"" + version_ +
                             "\",\"reason\":\"didn't start\"}";
            targetVersion_ = "";
        } else {
            const uint8_t attempts = p.getUChar("attempts", 0) + 1;
            p.putUChar("attempts", attempts);
            p.end();
            if (attempts > MAX_BOOT_ATTEMPTS) {
                rollBack("restarted " + String(attempts - 1) + " times before becoming healthy");
                return true;  // not reached: rollBack restarts
            }
            state_ = State::Confirming;
            confirmUntil_ = millis() + confirmTimeoutS_ * 1000;
            return true;
        }
    }
    p.end();
    return true;
}

// Point the bootloader back at the previous firmware, leave a note for it, restart
void OtaFeature::rollBack(const String& reason) {
    Preferences p;
    p.begin("ota", false);
    const String prev = p.getString("prev", "");
    const String prevVersion = p.getString("prevver", "");
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
    p.putBool("pending", false);
    p.putUChar("attempts", 0);
    const String error = "rolled back from " + version_ + ": " + reason;
    p.putString("lasterr", error);
    const String detail = "{\"version\":\"" + version_ + "\",\"previous_version\":\"" + prevVersion +
                          "\",\"reason\":\"" + reason + "\"}";
    p.putString("evt", "update_rolled_back");
    p.putString("evtdetail", detail);
    p.end();
    if (!part || esp_ota_set_boot_partition(part) != ESP_OK) {
        lastError_ = "rollback failed: previous firmware slot '" + prev + "' not usable";
        state_ = State::Failed;
        return;
    }
    host_->stopRecording("reboot");
    delay(300);
    ESP.restart();
}

// Remember what's being installed, so the new firmware can confirm itself or roll back
void OtaFeature::markPending(bool needMaster) {
    Preferences p;
    p.begin("ota", false);
    p.putBool("pending", true);
    p.putString("prev", esp_ota_get_running_partition()->label);
    p.putString("prevver", version_);
    p.putString("target", targetVersion_);
    p.putBool("needmaster", needMaster);
    p.putUChar("attempts", 0);
    p.putString("lasterr", "");
    p.end();
}

// ------------------------------------------------------------------ writing an image

void OtaFeature::startWrite() {
    scanTail_ = "";
    foundModel_ = "";
    foundVersion_ = "";
}

bool OtaFeature::write(const uint8_t* data, size_t n) {
    if (foundModel_.isEmpty()) {
        // The marker can straddle two chunks: search the previous chunk's tail plus this one
        std::vector<uint8_t> buf(scanTail_.length() + n);
        memcpy(buf.data(), scanTail_.c_str(), scanTail_.length());
        memcpy(buf.data() + scanTail_.length(), data, n);
        foundModel_ = fieldAfter(buf.data(), buf.size(), MARKER);
        if (!foundModel_.isEmpty()) foundVersion_ = fieldAfter(buf.data(), buf.size(), "VLFW-VERSION=");
        const size_t keep = min(MARKER_TAIL, buf.size());
        scanTail_ = "";
        for (size_t i = buf.size() - keep; i < buf.size(); i++) scanTail_ += buf[i] ? char(buf[i]) : '\x01';
    }
    return Update.write(const_cast<uint8_t*>(data), n) == n;
}

bool OtaFeature::finishWrite(String& error) {
    if (foundModel_.isEmpty()) {
        Update.abort();
        error = "not a logger firmware image (no VLFW-MODEL marker)";
        return false;
    }
    if (*host_->model() && foundModel_ != host_->model()) {
        Update.abort();
        error = "firmware is for " + foundModel_ + ", this logger is " + host_->model();
        return false;
    }
    if (!Update.end(true)) {  // checks the image and makes it the one to boot
        error = String("image rejected: ") + Update.errorString();
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ from the master

CmdStatus OtaFeature::command(const char* cmd, JsonObjectConst args, JsonObject result, String& message) {
    if (strcmp(cmd, "update") != 0) return CmdStatus::Unsupported;
    if (host_->recording()) {
        message = "recording: stop it first";
        return CmdStatus::Busy;
    }
    if (busy()) {
        message = "an update is already running";
        return CmdStatus::Busy;
    }
    const char* model = args["model"] | "";
    const char* version = args["version"] | "";
    const char* sha = args["sha256"] | "";
    const size_t size = args["size"] | 0;
    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    if (!*version || strlen(sha) != 64 || !size) {
        message = "version, sha256 (64 hex digits) and size are required";
        return CmdStatus::InvalidArgs;
    }
    if (*host_->model() && strcmp(model, host_->model()) != 0) {
        message = String("firmware is for ") + model + ", this logger is " + host_->model();
        return CmdStatus::InvalidArgs;
    }
    if (!slot || size > slot->size) {
        message = "image larger than the firmware slot";
        return CmdStatus::InvalidArgs;
    }
    String url = args["url"] | "";
    if (url.isEmpty()) {
        const String host = host_->net() ? host_->net()->masterHost() : String();
        if (host.isEmpty()) {
            message = "not connected to the master; give url";
            return CmdStatus::InvalidArgs;
        }
        url = "http://" + host + ":" + String(args["port"] | 8080) + String(args["path"] | "");
    }
    url_ = url;
    sha256_ = sha;
    sha256_.toLowerCase();
    size_ = size;
    targetVersion_ = version;
    lastError_ = "";
    progress_ = 0;
    state_ = State::Downloading;
    raise("update_started", "{\"version\":\"" + targetVersion_ + "\"}");
    xTaskCreatePinnedToCore([](void* self) { static_cast<OtaFeature*>(self)->downloadTask(); }, "ota", 8192, this, 1,
                            &task_, 0);
    result["accepted"] = true;
    return CmdStatus::Ok;
}

// Own task: streams the image into the spare slot, hashing as it goes; poll() handles the result
void OtaFeature::downloadTask() {
    String error;
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(STALL_TIMEOUT_MS);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    bool started = false;

    if (!http.begin(client, url_)) {
        error = "bad URL " + url_;
    } else {
        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            error = "download failed: HTTP " + String(code);
        } else if (http.getSize() != int(size_)) {
            error = "server says " + String(http.getSize()) + " bytes, expected " + String(size_);
        } else if (!Update.begin(size_)) {
            error = String("can't start writing: ") + Update.errorString();
        } else {
            started = true;
            startWrite();
            WiFiClient* stream = http.getStreamPtr();
            static uint8_t buf[4096];
            size_t done = 0;
            uint32_t lastData = millis();
            while (done < size_ && error.isEmpty()) {
                const size_t avail = stream->available();
                if (!avail) {
                    if (!http.connected() || millis() - lastData > STALL_TIMEOUT_MS) error = "download stalled";
                    else vTaskDelay(1);
                    continue;
                }
                const size_t n = stream->readBytes(buf, min(min(avail, sizeof(buf)), size_ - done));
                if (!n) continue;
                lastData = millis();
                mbedtls_sha256_update_ret(&sha, buf, n);
                if (!write(buf, n)) error = String("flash write failed: ") + Update.errorString();
                done += n;
                progress_ = uint8_t(done * 100 / size_);
            }
        }
        http.end();
    }

    if (error.isEmpty()) {
        state_ = State::Verifying;
        uint8_t digest[32];
        mbedtls_sha256_finish_ret(&sha, digest);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        if (sha256_ != hex) error = "SHA-256 mismatch (corrupted download)";
    }
    mbedtls_sha256_free(&sha);
    if (error.isEmpty()) {
        finishWrite(error);
    } else if (started) {
        Update.abort();
    }
    lastError_ = error;
    taskOk_ = error.isEmpty();
    taskDone_ = true;
    vTaskDelete(nullptr);
}

// ------------------------------------------------------------------ from the logger's page

bool OtaFeature::uploadBegin(String& error) {
    if (host_->recording()) {
        error = "recording: stop it first";
        return false;
    }
    if (busy()) {
        error = "an update is already running";
        return false;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        error = String("can't start writing: ") + Update.errorString();
        return false;
    }
    startWrite();
    targetVersion_ = "";
    lastError_ = "";
    progress_ = 0;
    state_ = State::Downloading;
    return true;
}

bool OtaFeature::uploadWrite(const uint8_t* data, size_t n) {
    if (state_ != State::Downloading) return false;
    if (write(data, n)) return true;
    lastError_ = String("flash write failed: ") + Update.errorString();
    Update.abort();
    state_ = State::Failed;
    return false;
}

bool OtaFeature::uploadEnd(String& error) {
    if (state_ != State::Downloading) {
        error = lastError_.isEmpty() ? String("upload failed") : lastError_;
        return false;
    }
    state_ = State::Verifying;
    if (!finishWrite(error)) {
        lastError_ = error;
        state_ = State::Failed;
        return false;
    }
    targetVersion_ = foundVersion_;
    markPending(false);
    state_ = State::Rebooting;
    rebootAt_ = millis() + 1500;
    return true;
}

void OtaFeature::uploadAbort() {
    if (state_ != State::Downloading) return;
    Update.abort();
    lastError_ = "upload interrupted";
    state_ = State::Failed;
}

bool OtaFeature::busy() const {
    return state_ == State::Downloading || state_ == State::Verifying || state_ == State::Rebooting;
}

// ------------------------------------------------------------------ main loop

// Events wait until they can reach the master (a rollback's report comes from the next boot)
void OtaFeature::raise(const char* name, const String& detail) {
    pendingEvent_ = name;
    pendingDetail_ = detail;
}

void OtaFeature::poll() {
    const uint32_t now = millis();
    Net* net = host_->net();

    if (!pendingEvent_.isEmpty() && (!net || net->connected())) {
        JsonDocument d;
        deserializeJson(d, pendingDetail_);
        host_->event(pendingEvent_.c_str(), instance(), d.as<JsonObjectConst>());
        pendingEvent_ = "";
        host_->statusChanged();
    }

    if (taskDone_) {
        taskDone_ = false;
        task_ = nullptr;
        if (taskOk_) {
            markPending(true);
            state_ = State::Rebooting;
            rebootAt_ = now + 1500;  // time for the status to reach the master
        } else {
            state_ = State::Failed;
            JsonDocument d;
            d["version"] = targetVersion_;
            d["error"] = lastError_;
            host_->event("update_failed", instance(), d.as<JsonObjectConst>());
        }
        host_->statusChanged();
    }
    if (state_ == State::Downloading) host_->statusChanged();  // progress, rate-limited by the link

    if (state_ == State::Rebooting && rebootAt_ && int32_t(now - rebootAt_) >= 0) {
        host_->stopRecording("reboot");
        ESP.restart();
    }

    if (state_ == State::Confirming) {
        const bool healthy = needMaster_ ? (net && net->connected()) : now >= PAGE_CONFIRM_MS;
        if (healthy) {
            Preferences p;
            p.begin("ota", false);
            p.putBool("pending", false);
            p.putUChar("attempts", 0);
            p.end();
            state_ = State::Idle;
            JsonDocument d;
            d["version"] = version_;
            d["previous_version"] = previousVersion_;
            host_->event("update_installed", instance(), d.as<JsonObjectConst>());
            targetVersion_ = "";
            host_->statusChanged();
        } else if (int32_t(now - confirmUntil_) >= 0) {
            rollBack(String(needMaster_ ? "didn't reach the master" : "didn't keep running") + " within " +
                     String(confirmTimeoutS_) + " s");
        }
    }
}

void OtaFeature::status(JsonObject out) const {
    out["state"] = stateName(state_);
    out["version"] = version_;
    if (state_ == State::Downloading) out["progress_pct"] = progress_;
    if (!targetVersion_.isEmpty()) out["target_version"] = targetVersion_;
    if (!previousVersion_.isEmpty()) out["previous_version"] = previousVersion_;
    if (state_ == State::Confirming) out["confirm_s"] = max(0, int32_t(confirmUntil_ - millis()) / 1000);
    if (!lastError_.isEmpty()) out["last_error"] = lastError_;
}

}  // namespace logger
