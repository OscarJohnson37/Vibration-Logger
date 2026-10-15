#include "dlog_writer.h"

#include <string.h>

namespace logger {

namespace {

constexpr uint8_t MAGIC[8] = {0x89, 'D', 'L', 'G', '\r', '\n', 0x1A, '\n'};
constexpr uint8_t REC_METADATA = 0x01;
constexpr uint8_t REC_DATA = 0x02;
constexpr uint8_t REC_MARKER = 0x03;
constexpr uint8_t REC_END = 0x7E;
constexpr size_t DATA_HEADER = 24;

uint32_t crcTable[256];
bool crcReady = false;

void initCrc() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[i] = c;
    }
    crcReady = true;
}

inline void putLe(uint8_t* p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = uint8_t(v >> (8 * i));
}

}  // namespace

const char* dtypeName(Dtype d) {
    switch (d) {
        case Dtype::I16: return "i16";
        case Dtype::I24: return "i24";
        case Dtype::I32: return "i32";
        case Dtype::U16: return "u16";
        case Dtype::U32: return "u32";
        case Dtype::F32: return "f32";
    }
    return "?";
}

size_t dtypeSize(Dtype d) {
    switch (d) {
        case Dtype::I16: case Dtype::U16: return 2;
        case Dtype::I24: return 3;
        default: return 4;
    }
}

// Standard reflected CRC32 with pre/post inversion; chain calls by passing the previous result.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    if (!crcReady) initCrc();
    crc = ~crc;
    while (len--) crc = crcTable[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void DlogWriter::put(const uint8_t* p, size_t n) {
    size_t w = out_.write(p, n);
    written_ += w;
    if (w < n) shortWrites_++;
}

void DlogWriter::record(uint8_t type, const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen) {
    uint8_t head[8] = {0xA5, 0x5A, type, 0};
    putLe(head + 4, aLen + bLen, 4);
    uint32_t crc = crc32Update(0, head + 2, 6);
    if (aLen) crc = crc32Update(crc, a, aLen);
    if (bLen) crc = crc32Update(crc, b, bLen);
    uint8_t tail[4];
    putLe(tail, crc, 4);
    put(head, 8);
    if (aLen) put(a, aLen);
    if (bLen) put(b, bLen);
    put(tail, 4);
}

void DlogWriter::begin(const char* metadataJson, size_t len) {
    uint8_t pre[16] = {};
    memcpy(pre, MAGIC, 8);
    putLe(pre + 8, 1, 2);   // version major
    putLe(pre + 10, 0, 2);  // version minor
    put(pre, 16);
    record(REC_METADATA, reinterpret_cast<const uint8_t*>(metadataJson), len);
}

void DlogWriter::data(const SampleBlock& b) {
    const StreamInfo& s = *b.stream;
    const size_t size = dtypeSize(s.dtype);
    const size_t nValues = size_t(b.nFrames) * s.nChannels;
    scratch_.resize(DATA_HEADER + nValues * size);
    uint8_t* p = scratch_.data();

    putLe(p, s.id, 2);
    putLe(p + 2, b.flags, 2);
    putLe(p + 4, b.nFrames, 4);
    putLe(p + 8, b.firstFrame, 8);
    putLe(p + 16, b.tMonoUs, 8);
    p += DATA_HEADER;

    if (s.dtype == Dtype::F32) {
        memcpy(p, b.f32, nValues * 4);  // ESP32 is little-endian
    } else {
        for (size_t i = 0; i < nValues; i++, p += size) putLe(p, uint32_t(b.i32[i]), size);
    }
    record(REC_DATA, scratch_.data(), scratch_.size());
}

void DlogWriter::marker(uint64_t tMonoUs, const char* json, size_t len) {
    uint8_t t[8];
    putLe(t, tMonoUs, 8);
    record(REC_MARKER, t, 8, reinterpret_cast<const uint8_t*>(json), len);
}

void DlogWriter::end() {
    record(REC_END, nullptr, 0);
}

}  // namespace logger
