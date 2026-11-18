#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include "providers/http.h"

namespace buddy {

struct TranscriptionResult {
    bool ok = false;
    std::string text;    // the recognised speech, trimmed; never empty when ok
    std::string error;   // written for the user; empty when ok
    HttpOutcome transport_outcome = HttpOutcome::Completed;
    int http_status = 0;
};

// Speech to text. Backends are interchangeable behind this (PROJECT.md feature 3).
class ISpeechToText {
public:
    virtual ~ISpeechToText() = default;

    // Blocks until the audio is transcribed, fails, or `stop` fires. Run it on a worker thread.
    // `wav` is one complete RIFF/WAVE file.
    virtual TranscriptionResult transcribe(std::span<const std::uint8_t> wav, std::stop_token stop) = 0;
};

struct TranscriptionConfig {
    std::string base_url;  // e.g. http://127.0.0.1:8000/v1
    std::string model;
};

// The OpenAI-compatible POST {base_url}/audio/transcriptions, for a server the user runs on this
// PC. Anything but a loopback address is refused, so recorded audio never leaves the machine.
class LocalTranscriber final : public ISpeechToText {
public:
    LocalTranscriber(IHttpTransport& transport, TranscriptionConfig config);

    // Takes effect for the next transcribe() call. Safe to call while one is running.
    void configure(TranscriptionConfig config);

    TranscriptionResult transcribe(std::span<const std::uint8_t> wav, std::stop_token stop) override;

private:
    IHttpTransport& transport_;
    mutable std::mutex mutex_;
    TranscriptionConfig config_;
};

std::string build_transcription_multipart(std::string_view boundary, std::string_view model,
                                          std::span<const std::uint8_t> wav);

}  // namespace buddy
