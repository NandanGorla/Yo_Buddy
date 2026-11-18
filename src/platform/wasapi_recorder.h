#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/audio_recorder.h"

namespace buddy {

// Records the default microphone through WASAPI in shared mode, on its own thread, and mixes
// whatever the device delivers down to mono 16-bit samples at the device's own sample rate.
//
// The device is opened on the capture thread, so start() returns before it is known whether the
// microphone works. A failure ends the recording: recording() turns false and error() says why.
class WasapiRecorder final : public IAudioRecorder {
public:
    struct Options {
        int max_duration_ms = 30'000;   // the recording ends by itself after this long
    };

    explicit WasapiRecorder(Options options = {});
    ~WasapiRecorder() override;

    WasapiRecorder(const WasapiRecorder&) = delete;
    WasapiRecorder& operator=(const WasapiRecorder&) = delete;

    bool start() override;
    std::vector<std::int16_t> stop() override;
    bool recording() const override;
    std::string error() const override;
    int sample_rate() const override;

private:
    void capture();

    Options options_;
    mutable std::mutex mutex_;
    std::thread thread_;
    std::vector<std::int16_t> samples_;
    std::string error_;
    int sample_rate_ = 16'000;
    bool recording_ = false;
    bool stop_requested_ = false;
};

}  // namespace buddy
