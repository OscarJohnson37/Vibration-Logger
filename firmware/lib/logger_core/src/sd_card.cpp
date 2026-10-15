#include "sd_card.h"

#include <SD_MMC.h>

#include "logger.h"

namespace logger {

bool SdCard::begin(Logger&, JsonObjectConst config) {
    oneBit_ = (config["bus_width"] | 4) == 1;
    freqKhz_ = constrain(config["freq_khz"] | 20000, 400, 40000);
    if (oneBit_) SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0);
    else SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0, pins_.d1, pins_.d2, pins_.d3);
    mount();
    return true;
}

bool SdCard::mount() {
    lastMountTry_ = millis();
    if (!SD_MMC.begin("/sdcard", oneBit_, false, freqKhz_, 2) || SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        mountFailures_++;
        return false;
    }
    totalMb_ = SD_MMC.totalBytes() >> 20;
    freeMb_ = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
    lastSpaceCheck_ = millis();
    mounted_ = true;
    return true;
}

void SdCard::poll() {
    uint32_t now = millis();
    if (!mounted_ && now - lastMountTry_ > 5000) mount();  // card inserted later, or back after an error
    if (mounted_ && now - lastSpaceCheck_ > 60000) {
        lastSpaceCheck_ = now;
        freeMb_ = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
    }
}

bool SdCard::wantsRestart() {
    if (needReopen_ && mounted_) {
        needReopen_ = false;
        return true;
    }
    return false;
}

void SdCard::status(JsonObject out) const {
    out["present"] = bool(mounted_);
    out["free_mb"] = freeMb_;
    out["total_mb"] = totalMb_;
    out["write_errors"] = writeErrors_;
    out["bus_width"] = oneBit_ ? 1 : 4;
    if (fileOpen_) out["file"] = path_;
}

// ------------------------------------------------------------------ sink (writer task)

bool SdCard::open(const String& metadataJson, const String& deviceId, uint32_t bootId, uint32_t fileIndex) {
    if (!mounted_) return false;
    String dir = "/logs/" + deviceId;
    SD_MMC.mkdir("/logs");
    SD_MMC.mkdir(dir);
    char name[32];
    snprintf(name, sizeof(name), "/b%06u_f%04u.dlog", unsigned(bootId), unsigned(fileIndex));
    path_ = dir + name;
    file_ = SD_MMC.open(path_, FILE_WRITE);
    if (!file_) {
        fail("open");
        return false;
    }
    fileOpen_ = true;
    fileStart_ = writer_.bytesWritten();
    writer_.begin(metadataJson.c_str(), metadataJson.length());
    file_.flush();  // the header is on the card before any data
    checkWrites();
    return fileOpen_;
}

void SdCard::write(const SampleBlock& b) {
    if (!fileOpen_) return;
    writer_.data(b);
    checkWrites();
}

void SdCard::marker(uint64_t tMonoUs, const String& json) {
    if (!fileOpen_) return;
    writer_.marker(tMonoUs, json.c_str(), json.length());
    checkWrites();
}

void SdCard::flush() {
    if (fileOpen_) file_.flush();
}

void SdCard::close() {
    if (!fileOpen_) return;
    writer_.end();
    file_.close();
    fileOpen_ = false;
}

void SdCard::checkWrites() {
    totalBytes_ = writer_.bytesWritten();
    if (writer_.shortWrites() != shortWritesSeen_) {
        shortWritesSeen_ = writer_.shortWrites();
        fail("write");
    }
}

void SdCard::fail(const char*) {
    writeErrors_++;
    if (file_) file_.close();
    fileOpen_ = false;
    SD_MMC.end();
    mounted_ = false;
    needReopen_ = true;  // poll() remounts; the host then opens a new file
}

}  // namespace logger
