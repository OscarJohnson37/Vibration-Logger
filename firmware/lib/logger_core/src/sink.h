// Destinations for recorded data. Every sink receives the same DLOG stream; which sinks a logger
// has is decided by its firmware, and which are used by config (recording.sinks). Nothing may
// assume an SD card: an Ethernet logger may have only a network sink. See docs/PLAN.md.
//
// Threading: open/write/marker/flush/close are only ever called from the Logger's writer task,
// in order. available/wantsRestart/poll are called from the main loop.
#pragma once

#include <Arduino.h>

#include "dlog_writer.h"

namespace logger {

class Sink {
public:
    virtual ~Sink() = default;
    virtual const char* name() const = 0;  // as used in recording.sinks: "sd", "usb", "netstream"

    // Can it take data right now? (card inserted and mounted, link up, …)
    virtual bool available() { return true; }

    // Start a new file/stream: preamble + metadata. Returns false if it couldn't.
    // session and fileName give the file's place (FILE_FORMAT.md, Files): /logs/<session>/<fileName>
    virtual bool open(const String& metadataJson, const String& session, const String& fileName) = 0;
    virtual void write(const SampleBlock& b) = 0;
    virtual void marker(uint64_t tMonoUs, const String& json) = 0;
    virtual void flush() {}
    virtual void close() = 0;  // END + close

    // Ask the host to reopen this sink with fresh metadata (USB capture starting, SD card back)
    virtual bool wantsRestart() { return false; }
    virtual void requestRestart() {}
    virtual void poll() {}
    virtual size_t bytesWritten() const { return 0; }
    virtual size_t fileBytes() const { return 0; }  // bytes in the current file, for file_max_mb
};

// Streams DLOG over the USB serial port, for bench capture with `python -m logger_reader capture`.
// Data is dropped (not buffered) when no PC is reading, so it never stalls recording.
class UsbSink : public Sink {
public:
    const char* name() const override { return "usb"; }
    bool open(const String& metadataJson, const String&, const String&) override {
        writer_.begin(metadataJson.c_str(), metadataJson.length());
        return true;
    }
    void write(const SampleBlock& b) override { writer_.data(b); }
    void marker(uint64_t tMonoUs, const String& json) override { writer_.marker(tMonoUs, json.c_str(), json.length()); }
    void close() override { writer_.end(); }
    bool wantsRestart() override { bool r = restart_; restart_ = false; return r; }
    void requestRestart() override { restart_ = true; }
    size_t bytesWritten() const override { return writer_.bytesWritten(); }

private:
    DlogWriter writer_{Serial};
    volatile bool restart_ = false;
};

}  // namespace logger
