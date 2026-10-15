// Writes a DLOG v1 byte stream (protocol/FILE_FORMAT.md) to any Arduino Print:
// a USB serial port, an SD card file or a TCP connection.
#pragma once

#include <Arduino.h>
#include <vector>

#include "stream_info.h"

namespace logger {

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);  // IEEE, same as zlib

class DlogWriter {
public:
    explicit DlogWriter(Print& out) : out_(out) {}

    void begin(const char* metadataJson, size_t len);  // preamble + METADATA
    void data(const SampleBlock& b);
    void marker(uint64_t tMonoUs, const char* json, size_t len);
    void end();

    size_t bytesWritten() const { return written_; }
    uint32_t shortWrites() const { return shortWrites_; }  // output accepted fewer bytes than given

private:
    void record(uint8_t type, const uint8_t* a, size_t aLen, const uint8_t* b = nullptr, size_t bLen = 0);
    void put(const uint8_t* p, size_t n);

    Print& out_;
    size_t written_ = 0;
    uint32_t shortWrites_ = 0;
    std::vector<uint8_t> scratch_;
};

}  // namespace logger
