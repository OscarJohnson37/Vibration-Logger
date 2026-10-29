// The `publish` feature (protocol/features/publish.json, MQTT.md "Data publications"): live
// summaries of a stream on loggers/<id>/data/<name>, e.g. accel_1s = per-axis mean, AC RMS and
// peak every second. A live view only: sent while connected, never queued, and never touching
// what's recorded.
//
// Methods: latest, mean, stats (mean, min, max, rms, ac_rms, peak, peak_to_peak, crest).
// Windows equal the interval; axes are the sensor's own ("sensor"). Publications run whether or
// not the logger is recording: the host keeps acquisition running while any is enabled.
//
// Config (publish section): "budget_kbps": 32,
//   "publications": [{ "name", "stream", "method", "params": { "stats": [...] }, "interval_s",
//                      "qos", "retain", "enabled" }]
#pragma once

#include <freertos/FreeRTOS.h>

#include "feature.h"

namespace logger {

class PublishFeature : public Feature {
public:
    PublishFeature() : Feature("publish", "publish", 1) {}

    bool begin(Logger& host, JsonObjectConst config) override;
    void describe(JsonObject info) const override;
    void status(JsonObject out) const override;
    bool wantsSamples() const override;
    void consume(const SampleBlock& b) override;  // acquisition task
    void poll() override;                         // main loop

private:
    enum Method : uint8_t { Latest, Mean, Stats };
    enum Stat : uint8_t { SMean, SMin, SMax, SRms, SAcRms, SPeak, SPeakToPeak, SCrest, NSTATS };

    // Running sums over one window, in raw units
    struct Acc {
        uint32_t n = 0;
        uint64_t tMonoUs = 0;
        uint16_t flags = 0;
        double sum[MAX_CHANNELS], sumSq[MAX_CHANNELS], min[MAX_CHANNELS], max[MAX_CHANNELS], last[MAX_CHANNELS];
        void reset(uint8_t nCh);
    };

    struct Publication {
        String name, stream;
        Method method = Stats;
        uint16_t stats = 0;  // bit per Stat
        uint32_t intervalMs = 1000;
        uint8_t qos = 0;
        bool retain = false;
        const StreamInfo* source = nullptr;  // found on the first block from the named stream
        Acc acc;
        uint32_t lastSent = 0;
        uint32_t sent = 0, skipped = 0;
    };

    void send(Publication& p, const Acc& a);

    static constexpr size_t MAX_PUBLICATIONS = 4;
    Logger* host_ = nullptr;
    Publication pubs_[MAX_PUBLICATIONS];
    size_t nPubs_ = 0;
    uint32_t budgetBytesPerS_ = 32 * 1024;
    uint32_t budgetWindowStart_ = 0, budgetUsed_ = 0;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace logger
