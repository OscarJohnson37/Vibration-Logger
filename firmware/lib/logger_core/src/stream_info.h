// Stream and sample-block descriptions shared by features, sinks and the DLOG writer.
// See protocol/FILE_FORMAT.md (Metadata, DATA records).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace logger {

enum class Dtype : uint8_t { I16, I24, I32, U16, U32, F32 };

const char* dtypeName(Dtype d);
size_t dtypeSize(Dtype d);

struct ChannelInfo {
    const char* name = "";
    const char* unit = "";
    double scale = 1.0;
    double offset = 0.0;
};

constexpr size_t MAX_CHANNELS = 8;

struct StreamInfo {
    uint16_t id = 0;            // assigned by the Logger when the feature is added
    const char* name = "";
    const char* feature = "";   // feature instance that produces it
    double rateHz = 0;
    Dtype dtype = Dtype::I32;
    bool hasRawRange = false;
    int32_t rawMin = 0;
    int32_t rawMax = 0;
    uint8_t nChannels = 0;
    ChannelInfo channels[MAX_CHANNELS];
};

// DATA block flags (FILE_FORMAT.md)
constexpr uint16_t FLAG_CLIPPED = 1 << 0;
constexpr uint16_t FLAG_GAP = 1 << 1;
constexpr uint16_t FLAG_SUSPECT = 1 << 2;

// One block of consecutive frames from one stream. Exactly one of i32/f32 is set:
// integer dtypes use i32 (raw counts), F32 uses f32. Values are frame-major.
struct SampleBlock {
    const StreamInfo* stream = nullptr;
    uint16_t flags = 0;
    uint32_t nFrames = 0;
    uint64_t firstFrame = 0;
    uint64_t tMonoUs = 0;
    const int32_t* i32 = nullptr;
    const float* f32 = nullptr;
};

}  // namespace logger
