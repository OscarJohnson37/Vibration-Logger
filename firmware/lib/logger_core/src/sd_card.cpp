#include "sd_card.h"

#include <SD_MMC.h>
#include <sdmmc_cmd.h>
#include <driver/gpio.h>

#include "logger.h"

namespace logger {

namespace {
// SDMMCFS keeps the card handle protected; the status check needs it
struct SdmmcCard : fs::SDMMCFS {
    static sdmmc_card_t* of(fs::SDMMCFS& fs) { return static_cast<SdmmcCard&>(fs)._card; }
};
}  // namespace

// The recording, plus self_test's test file / directory listing / newest-recording check
constexpr uint8_t MAX_OPEN_FILES = 4;

bool SdCard::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;  // before mount(), which can raise sd_full
    // D1-D3 not wired (-1): only 1-bit is possible
    oneBit_ = (config["bus_width"] | 4) == 1 || pins_.d1 < 0 || pins_.d2 < 0 || pins_.d3 < 0;
    freqKhz_ = constrain(config["freq_khz"] | 20000, 400, 40000);
    if (oneBit_) SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0);
    else SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0, pins_.d1, pins_.d2, pins_.d3);
    mount();
    return true;
}

bool SdCard::mount() {
    lastMountTry_ = millis();
    if (!SD_MMC.begin("/sdcard", oneBit_, false, freqKhz_, MAX_OPEN_FILES) || SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        mountFailures_++;
        return false;
    }
    mounted_ = true;
    updateSpace();
    return true;
}

void SdCard::updateSpace() {
    lastSpaceCheck_ = millis();
    totalMb_ = SD_MMC.totalBytes() >> 20;
    freeMb_ = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
    const bool full = totalMb_ > 0 && freeMb_ * 100 < totalMb_;
    if (full && !full_ && host_) {
        JsonDocument d;
        d["free_mb"] = freeMb_;
        host_->event("sd_full", instance(), d.as<JsonObjectConst>());
    }
    full_ = full;
}

// CMD13 (SEND_STATUS): answers in microseconds if the card is there. The SDMMC driver serialises
// commands, so this is safe alongside the writer task's writes.
bool SdCard::cardResponds() {
    sdmmc_card_t* card = SdmmcCard::of(SD_MMC);
    return card && sdmmc_get_status(card) == ESP_OK;
}

void SdCard::poll() {
    uint32_t now = millis();
    if (!mounted_ && now - lastMountTry_ > 5000) mount();  // card inserted later, or back after an error
    if (mounted_ && !fileOpen_ && now - lastPresenceCheck_ >= 2000) {
        // While recording, a missing card shows up as a failed write (writer task); idle, ask it
        lastPresenceCheck_ = now;
        if (!cardResponds()) {
            SD_MMC.end();
            mounted_ = false;
            full_ = false;
            totalMb_ = freeMb_ = 0;
            lastMountTry_ = now;
            host_->event("sd_error", instance());
            return;
        }
    }
    if (mounted_ && now - lastSpaceCheck_ > 60000) updateSpace();
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
    if (full_) out["full"] = true;
    out["bus_width"] = oneBit_ ? 1 : 4;
    if (fileOpen_) out["file"] = path_;
}

// Wiring check on the SD pins as plain GPIOs (card unmounted):
//   idle     level with only the ESP32's weak pull-down: 1 = held high by the card or breakout
//            pull-ups (joint OK); 0 = nothing holding it high (open joint, or no pull-up there)
//   driven   the pin driven high, then low: "stuck" = shorted to GND or 3V3
//   bridged  other SD pins that followed it (high if they idle low, low if they idle high)
void SdCard::pinCheck(JsonObject out) {
    const char* names[6] = {"clk", "cmd", "d0", "d1", "d2", "d3"};
    const int pins[6] = {pins_.clk, pins_.cmd, pins_.d0, pins_.d1, pins_.d2, pins_.d3};
    int idle[6];
    for (int i = 0; i < 6; i++)
        if (pins[i] >= 0) pinMode(pins[i], INPUT_PULLDOWN);
    delay(5);
    for (int i = 0; i < 6; i++) idle[i] = pins[i] >= 0 ? digitalRead(pins[i]) : 0;
    for (int i = 0; i < 6; i++) {
        if (pins[i] < 0) continue;  // not wired (1-bit mode)
        JsonObject o = out[names[i]].to<JsonObject>();
        o["gpio"] = pins[i];
        o["idle"] = idle[i];
        bool follows[6] = {false};
        // Drive for microseconds only, so a short to GND or 3V3 does no harm
        pinMode(pins[i], OUTPUT);
        gpio_set_direction(gpio_num_t(pins[i]), GPIO_MODE_INPUT_OUTPUT);  // so it can read its own level
        digitalWrite(pins[i], HIGH);
        delayMicroseconds(20);
        const int high = digitalRead(pins[i]);
        for (int j = 0; j < 6; j++)
            if (j != i && pins[j] >= 0 && !idle[j] && digitalRead(pins[j])) follows[j] = true;
        digitalWrite(pins[i], LOW);
        delayMicroseconds(20);
        const int low = digitalRead(pins[i]);
        for (int j = 0; j < 6; j++)
            if (j != i && pins[j] >= 0 && idle[j] && !digitalRead(pins[j])) follows[j] = true;
        pinMode(pins[i], INPUT_PULLDOWN);
        delayMicroseconds(200);
        o["driven"] = high && !low ? "ok" : !high ? "stuck low (short to GND?)" : "stuck high (short to 3V3?)";
        JsonArray b = o["bridged"].to<JsonArray>();
        for (int j = 0; j < 6; j++)
            if (follows[j]) b.add(names[j]);
    }
}

// ------------------------------------------------------------------ self_test (main loop)
//
// Runs alongside recording: FATFS locks per call, and the test only touches its own file and
// reads the recordings. Failures are reported, never passed to fail() (that's the writer's).

CmdStatus SdCard::command(const char* cmd, JsonObjectConst, JsonObject result, String& message) {
    if (strcmp(cmd, "self_test") != 0) return CmdStatus::Unsupported;
    if (!mounted_ && !mount()) {
        // Narrow down a wiring fault: retry slowly in 4-bit, then 1-bit (CLK, CMD, D0 only)
        auto probe = [&](bool oneBit) {
            if (oneBit) SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0);
            else SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0, pins_.d1, pins_.d2, pins_.d3);
            bool ok = SD_MMC.begin("/sdcard", oneBit, false, 400, MAX_OPEN_FILES) && SD_MMC.cardType() != CARD_NONE;
            SD_MMC.end();
            return ok;
        };
        const bool slow4 = !oneBit_ && probe(false);
        const bool slow1 = !slow4 && probe(true);
        pinCheck(result["pins"].to<JsonObject>());
        if (!oneBit_) SD_MMC.setPins(pins_.clk, pins_.cmd, pins_.d0, pins_.d1, pins_.d2, pins_.d3);
        result["mounted"] = false;
        result["passed"] = false;
        result["works_4bit_400khz"] = slow4;
        result["works_1bit_400khz"] = slow1;
        message = slow4   ? "card only works at 400 kHz: wiring too long or noisy, lower sd.freq_khz"
                  : slow1 ? "card works in 1-bit mode only: check D1, D2, D3 (CS) wiring"
                          : "no response: check the card is inserted, 3V and GND, then CLK and CMD (SI)";
        return CmdStatus::Ok;
    }
    result["mounted"] = true;
    const uint8_t type = SD_MMC.cardType();
    result["card_type"] = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC" : type == CARD_SDHC ? "SDHC/SDXC" : "unknown";
    result["bus_width"] = oneBit_ ? 1 : 4;
    result["freq_khz"] = freqKhz_;
    result["total_mb"] = SD_MMC.totalBytes() >> 20;
    result["free_mb"] = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;

    // 64 KB of a pattern that differs every byte and every run: write, close, read back, compare
    static uint8_t buf[4096];
    const uint32_t seed = esp_random();
    const size_t chunks = 16;
    auto fill = [&](size_t c) {
        for (size_t i = 0; i < sizeof(buf); i++) buf[i] = uint8_t((seed >> (i % 4 * 8)) + c * 131 + i * 7);
    };
    size_t written = 0, readBack = 0, badChunks = 0;
    uint32_t t0 = millis();
    File f = SD_MMC.open("/selftest.bin", FILE_WRITE);
    for (size_t c = 0; f && c < chunks; c++) {
        fill(c);
        written += f.write(buf, sizeof(buf));
    }
    if (f) f.close();
    const uint32_t writeMs = millis() - t0;
    static uint8_t rd[4096];
    t0 = millis();
    f = SD_MMC.open("/selftest.bin");
    for (size_t c = 0; f && c < chunks; c++) {
        size_t n = f.read(rd, sizeof(rd));
        readBack += n;
        fill(c);
        if (n != sizeof(rd) || memcmp(rd, buf, n) != 0) badChunks++;
    }
    if (f) f.close();
    const uint32_t readMs = millis() - t0;
    SD_MMC.remove("/selftest.bin");
    const bool rwOk = written == chunks * sizeof(buf) && readBack == written && badChunks == 0;
    result["write_bytes"] = written;
    result["read_bytes"] = readBack;
    result["bad_chunks"] = badChunks;
    result["write_kBps"] = writeMs ? written / 1.024f / writeMs : 0;
    result["read_kBps"] = readMs ? readBack / 1.024f / readMs : 0;
    result["read_write_ok"] = rwOk;

    // Recordings in the current session: count them, and check the newest starts with the DLOG preamble
    static const uint8_t MAGIC[8] = {0x89, 0x44, 0x4C, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};  // the DLOG preamble
    String dir = "/logs/" + (host_ ? host_->session() : String());
    File d = SD_MMC.open(dir);
    size_t count = 0;
    String newest;
    size_t newestSize = 0;
    // Close each entry before opening the next: every one counts against MAX_OPEN_FILES
    for (File e = d ? d.openNextFile() : File(); e; e = d.openNextFile()) {
        if (!e.isDirectory()) {
            count++;
            String name = e.name();
            if (name > newest) {
                newest = name;
                newestSize = e.size();
            }
        }
        e.close();
    }
    if (d) d.close();
    result["log_dir"] = dir;
    result["log_files"] = count;
    if (count) {
        result["newest"] = newest;
        result["newest_bytes"] = newestSize;
        uint8_t head[8] = {0};
        f = SD_MMC.open(dir + "/" + newest);
        const bool headOk = f && f.read(head, 8) == 8 && memcmp(head, MAGIC, 8) == 0;
        if (f) f.close();
        result["newest_is_dlog"] = headOk;
    }
    result["passed"] = rwOk && (!count || result["newest_is_dlog"].as<bool>());
    return CmdStatus::Ok;
}

// ------------------------------------------------------------------ sink (writer task)

bool SdCard::open(const String& metadataJson, const String& session, const String& fileName) {
    if (!mounted_) return false;
    String dir = "/logs/" + session;
    SD_MMC.mkdir("/logs");
    SD_MMC.mkdir(dir);
    path_ = dir + "/" + fileName;
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
