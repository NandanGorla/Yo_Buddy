#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include "platform/capture_target.h"

namespace buddy::capture {

struct Frame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> bgra;
};

struct FrameResult {
    Frame frame;
    std::string error;

    bool ok() const { return error.empty() && frame.width > 0 && frame.height > 0; }
};

// Captures one explicit frame through Windows Graphics Capture. The result is BGRA8 and is
// downscaled so a provider request cannot accidentally consume an unbounded desktop image.
FrameResult capture_one(const TargetResult& target, int max_dimension = 1920);

struct EncodedFrameResult {
    std::string base64;
    std::string error;

    bool ok() const { return error.empty() && !base64.empty(); }
};

EncodedFrameResult encode_png(const Frame& frame);

}  // namespace buddy::capture
