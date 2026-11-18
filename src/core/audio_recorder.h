#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace buddy {

// A microphone that records while asked to. The real one is WASAPI (platform/wasapi_recorder.h);
// tests use a fake, so the voice flow can be checked without a sound device.
class IAudioRecorder {
public:
    virtual ~IAudioRecorder() = default;

    // Starts recording. False when a recording is already running or cannot be started.
    virtual bool start() = 0;
    // Stops and returns what was recorded: mono, 16-bit, at sample_rate().
    virtual std::vector<std::int16_t> stop() = 0;
    // False once the recording has ended, whether by stop(), by its length limit or by an error.
    virtual bool recording() const = 0;
    // Why the last recording failed, written for the user. Empty when it did not.
    virtual std::string error() const = 0;
    virtual int sample_rate() const = 0;
};

}  // namespace buddy
