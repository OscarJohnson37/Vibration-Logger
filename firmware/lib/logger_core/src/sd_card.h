// SD card storage: both the `sd` feature (status, card presence) and the "sd" recording sink
// (DLOG files under /logs/<device_id>/, protocol/FILE_FORMAT.md). Uses the ESP32-S3 SDMMC host
// in 4-bit (or 1-bit) SDIO mode on any pins.
//
// Card removed or failing: the file is abandoned (the reader recovers everything up to the last
// complete record), the error colour shows, and when a card is mounted again the logger opens a
// new file and carries on.
//
// Config (sd section): "bus_width": 4 | 1, "freq_khz": 20000
#pragma once

#include <FS.h>

#include "feature.h"
#include "sink.h"

namespace logger {

class SdCard : public Feature, public Sink {
public:
    struct Pins {
        int clk, cmd, d0, d1, d2, d3;
    };
    explicit SdCard(const Pins& pins) : Feature("sd", "sd", 1), pins_(pins) {}

    // Feature
    bool begin(Logger& host, JsonObjectConst config) override;
    void status(JsonObject out) const override;
    bool ok() const override { return mounted_; }
    void poll() override;  // also Sink::poll

    // Sink
    const char* name() const override { return "sd"; }
    bool available() override { return mounted_; }
    bool open(const String& metadataJson, const String& deviceId, uint32_t bootId, uint32_t fileIndex) override;
    void write(const SampleBlock& b) override;
    void marker(uint64_t tMonoUs, const String& json) override;
    void flush() override;
    void close() override;
    bool wantsRestart() override;
    size_t bytesWritten() const override { return totalBytes_; }
    size_t fileBytes() const override { return fileOpen_ ? writer_.bytesWritten() - fileStart_ : 0; }

private:
    bool mount();
    void fail(const char* why);  // writer task: abandon the card until it's remounted
    void checkWrites();

    Pins pins_;
    bool oneBit_ = false;
    int freqKhz_ = 20000;

    volatile bool mounted_ = false;
    volatile bool needReopen_ = false;  // a file was lost to an error; reopen once remounted
    volatile bool fileOpen_ = false;
    uint32_t lastMountTry_ = 0;
    uint32_t lastSpaceCheck_ = 0;
    uint64_t totalMb_ = 0, freeMb_ = 0;
    uint32_t writeErrors_ = 0;
    uint32_t shortWritesSeen_ = 0;
    uint32_t mountFailures_ = 0;
    size_t totalBytes_ = 0;
    size_t fileStart_ = 0;
    String path_;

    fs::File file_;
    DlogWriter writer_{file_};
};

}  // namespace logger
