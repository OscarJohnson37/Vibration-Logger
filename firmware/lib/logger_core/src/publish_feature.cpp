#include "publish_feature.h"

#include <math.h>

#include <functional>

#include "logger.h"

namespace logger {

namespace {

const char* const STAT_NAMES[] = {"mean", "min", "max", "rms", "ac_rms", "peak", "peak_to_peak", "crest"};
const char* const METHOD_NAMES[] = {"latest", "mean", "stats"};

int indexOf(const char* const* names, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) return int(i);
    return -1;
}

}  // namespace

void PublishFeature::Acc::reset(uint8_t nCh) {
    n = 0;
    flags = 0;
    for (uint8_t c = 0; c < nCh; c++) {
        sum[c] = sumSq[c] = last[c] = 0;
        min[c] = INFINITY;
        max[c] = -INFINITY;
    }
}

bool PublishFeature::begin(Logger& host, JsonObjectConst config) {
    host_ = &host;
    budgetBytesPerS_ = uint32_t(max(1.0f, config["budget_kbps"] | 32.0f) * 1024);
    for (JsonObjectConst d : config["publications"].as<JsonArrayConst>()) {
        if (nPubs_ >= MAX_PUBLICATIONS || !(d["enabled"] | true)) continue;
        const int method = indexOf(METHOD_NAMES, 3, d["method"] | "");
        const char* name = d["name"] | "";
        const char* stream = d["stream"] | "";
        if (method < 0 || !*name || !*stream) continue;  // the master validates; skip what we can't do
        Publication& p = pubs_[nPubs_++];
        p.name = name;
        p.stream = stream;
        p.method = Method(method);
        for (JsonVariantConst s : d["params"]["stats"].as<JsonArrayConst>()) {
            const int i = indexOf(STAT_NAMES, NSTATS, s | "");
            if (i >= 0) p.stats |= 1 << i;
        }
        if (!p.stats) p.stats = (1 << SMean) | (1 << SAcRms) | (1 << SPeak);
        p.intervalMs = uint32_t(max(0.1f, d["interval_s"] | 1.0f) * 1000);
        p.qos = constrain(d["qos"] | 0, 0, 1);
        p.retain = d["retain"] | false;
        p.acc.reset(MAX_CHANNELS);
    }
    return true;
}

void PublishFeature::describe(JsonObject info) const {
    JsonArray m = info["methods"].to<JsonArray>();
    for (const char* name : METHOD_NAMES) m.add(name);
}

void PublishFeature::status(JsonObject out) const {
    JsonArray a = out["publications"].to<JsonArray>();
    for (size_t i = 0; i < nPubs_; i++) {
        JsonObject o = a.add<JsonObject>();
        o["name"] = pubs_[i].name;
        o["sent"] = pubs_[i].sent;
        o["skipped"] = pubs_[i].skipped;
    }
}

bool PublishFeature::wantsSamples() const {
    return nPubs_ > 0;
}

// Acquisition task: sums each block locally, then merges under the lock (a few dozen operations)
void PublishFeature::consume(const SampleBlock& b) {
    for (size_t i = 0; i < nPubs_; i++) {
        Publication& p = pubs_[i];
        if (!p.source) {
            if (p.stream != b.stream->name) continue;
            p.source = b.stream;
        }
        if (p.source != b.stream) continue;
        const uint8_t nCh = b.stream->nChannels;
        Acc local;
        local.reset(nCh);
        for (uint32_t f = 0; f < b.nFrames; f++) {
            for (uint8_t c = 0; c < nCh; c++) {
                const size_t k = size_t(f) * nCh + c;
                const double v = b.f32 ? double(b.f32[k]) : double(b.i32[k]);
                local.sum[c] += v;
                local.sumSq[c] += v * v;
                if (v < local.min[c]) local.min[c] = v;
                if (v > local.max[c]) local.max[c] = v;
                local.last[c] = v;
            }
        }
        portENTER_CRITICAL(&mux_);
        if (p.acc.n == 0) p.acc.tMonoUs = b.tMonoUs;
        p.acc.n += b.nFrames;
        p.acc.flags |= b.flags;
        for (uint8_t c = 0; c < nCh; c++) {
            p.acc.sum[c] += local.sum[c];
            p.acc.sumSq[c] += local.sumSq[c];
            p.acc.min[c] = fmin(p.acc.min[c], local.min[c]);
            p.acc.max[c] = fmax(p.acc.max[c], local.max[c]);
            p.acc.last[c] = local.last[c];
        }
        portEXIT_CRITICAL(&mux_);
    }
}

void PublishFeature::poll() {
    const uint32_t now = millis();
    for (size_t i = 0; i < nPubs_; i++) {
        Publication& p = pubs_[i];
        if (now - p.lastSent < p.intervalMs) continue;
        p.lastSent = now;
        if (!p.source) continue;
        Acc a;
        portENTER_CRITICAL(&mux_);
        a = p.acc;
        p.acc.reset(p.source->nChannels);
        portEXIT_CRITICAL(&mux_);
        if (a.n == 0 || !host_->net() || !host_->net()->connected()) continue;
        send(p, a);
    }
}

// Builds the message (MQTT.md, Message format), in engineering units: value = raw × scale + offset
void PublishFeature::send(Publication& p, const Acc& a) {
    const StreamInfo& s = *p.source;
    JsonDocument d;
    d["v"] = 1;
    const int64_t utc = host_->utcMs();
    if (utc >= 0) d["t"] = utc;
    else d["t"] = nullptr;
    d["uptime_ms"] = millis();
    d["name"] = p.name;
    d["stream"] = s.name;
    d["method"] = METHOD_NAMES[p.method];
    if (p.method == Stats) {
        JsonArray st = d["params"]["stats"].to<JsonArray>();
        for (int k = 0; k < NSTATS; k++)
            if (p.stats & (1 << k)) st.add(STAT_NAMES[k]);
    }
    JsonObject w = d["window"].to<JsonObject>();
    w["t_mono_us"] = a.tMonoUs;
    w["duration_s"] = s.rateHz > 0 ? a.n / s.rateHz : 0;
    w["frames"] = a.n;
    JsonArray fl = w["flags"].to<JsonArray>();
    if (a.flags & FLAG_CLIPPED) fl.add("clipped");
    if (a.flags & FLAG_GAP) fl.add("gap");
    if (a.flags & FLAG_SUSPECT) fl.add("suspect");
    JsonArray ch = d["channels"].to<JsonArray>();
    for (uint8_t c = 0; c < s.nChannels; c++) ch.add(s.channels[c].name);
    d["unit"] = s.channels[0].unit;
    d["axes"] = "sensor";

    JsonObject values = d["values"].to<JsonObject>();
    auto put = [&](const char* name, std::function<double(uint8_t)> fn) {
        JsonArray arr = values[name].to<JsonArray>();
        for (uint8_t c = 0; c < s.nChannels; c++) arr.add(float(fn(c)));
    };
    auto mean = [&](uint8_t c) { return a.sum[c] / a.n; };
    auto eng = [&](uint8_t c, double raw) { return raw * s.channels[c].scale + s.channels[c].offset; };
    auto acRms = [&](uint8_t c) {
        return fabs(s.channels[c].scale) * sqrt(fmax(0.0, a.sumSq[c] / a.n - mean(c) * mean(c)));
    };
    auto peak = [&](uint8_t c) { return fabs(s.channels[c].scale) * fmax(a.max[c] - mean(c), mean(c) - a.min[c]); };

    if (p.method == Latest) put("latest", [&](uint8_t c) { return eng(c, a.last[c]); });
    if (p.method == Mean) put("mean", [&](uint8_t c) { return eng(c, mean(c)); });
    if (p.method == Stats) {
        if (p.stats & (1 << SMean)) put("mean", [&](uint8_t c) { return eng(c, mean(c)); });
        if (p.stats & (1 << SMin))
            put("min", [&](uint8_t c) { return s.channels[c].scale >= 0 ? eng(c, a.min[c]) : eng(c, a.max[c]); });
        if (p.stats & (1 << SMax))
            put("max", [&](uint8_t c) { return s.channels[c].scale >= 0 ? eng(c, a.max[c]) : eng(c, a.min[c]); });
        if (p.stats & (1 << SRms))
            put("rms", [&](uint8_t c) {
                const double sc = s.channels[c].scale, o = s.channels[c].offset;
                return sqrt(fmax(0.0, sc * sc * a.sumSq[c] / a.n + 2 * sc * o * mean(c) + o * o));
            });
        if (p.stats & (1 << SAcRms)) put("ac_rms", acRms);
        if (p.stats & (1 << SPeak)) put("peak", peak);
        if (p.stats & (1 << SPeakToPeak))
            put("peak_to_peak", [&](uint8_t c) { return fabs(s.channels[c].scale) * (a.max[c] - a.min[c]); });
        if (p.stats & (1 << SCrest))
            put("crest", [&](uint8_t c) { return acRms(c) > 0 ? peak(c) / acRms(c) : 0.0; });
    }

    String json;
    serializeJson(d, json);
    const uint32_t now = millis();
    if (now - budgetWindowStart_ >= 1000) {
        budgetWindowStart_ = now;
        budgetUsed_ = 0;
    }
    if (budgetUsed_ + json.length() > budgetBytesPerS_ || !host_->net()->publish("data/" + p.name, json, p.qos, p.retain)) {
        p.skipped++;
        return;
    }
    budgetUsed_ += json.length();
    p.sent++;
}

}  // namespace logger
