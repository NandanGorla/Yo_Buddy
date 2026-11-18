#pragma once

#include <cstddef>
#include <stop_token>
#include <string>
#include <thread>

#include "providers/http.h"

namespace buddy {

// Reads text aloud on a worker thread.
//
// Windows' built-in voices (SAPI) are the default and need nothing installed. When a local
// OpenAI-compatible speech server is configured, it is tried first (POST {base_url}/audio/speech,
// WAV reply) and the Windows voice is the fallback when that server does not deliver.
//
// All methods must be called from the same thread (the UI thread). The worker checks for a stop
// request every few tens of milliseconds in every phase, so stop() returns quickly.
class SapiVoice final {
public:
    // Longer text is cut off: this is for short answers and captions.
    static constexpr std::size_t kMaxSpokenBytes = 2'000;

    struct Config {
        std::string voice;             // name of an installed Windows voice; "" is the system default
        int rate = 0;                  // -10..10, 0 is the voice's normal speed
        int pitch = 0;                 // -10..10, 0 is the voice's normal pitch
        std::string neural_base_url;   // "" means Windows voices only; must be a loopback URL
        std::string neural_model;
    };

    // `transport` is only used for the local neural server, and may be null.
    explicit SapiVoice(IHttpTransport* transport = nullptr);
    ~SapiVoice();

    SapiVoice(const SapiVoice&) = delete;
    SapiVoice& operator=(const SapiVoice&) = delete;

    void configure(Config config);   // takes effect from the next speak()
    void set_muted(bool muted);      // muting also silences what is being said
    void speak(std::string text);    // replaces whatever is being said
    void stop();

private:
    void run(const std::string& text, const Config& config, std::stop_token stop);
    bool speak_neural(const std::string& text, const Config& config, std::stop_token stop);

    IHttpTransport* transport_ = nullptr;
    Config config_;
    bool muted_ = false;
    std::thread thread_;
    std::stop_source stop_source_;
};

}  // namespace buddy
