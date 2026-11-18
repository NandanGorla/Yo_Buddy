#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "core/audio_recorder.h"
#include "providers/transcription.h"

namespace buddy {

// One push-to-talk round: record the microphone, transcribe the recording, hand over the text.
//
//   Idle/Error --start()--> Listening --stop()--> Transcribing --> Idle (text) or Error
//
// start(), stop() and cancel() must all be called from the same thread (the UI thread). None of
// them waits for a transcription: that runs on a worker, and only cancel() and the destructor
// wait for the worker, after telling it to stop.
class VoiceSession final {
public:
    enum class State { Idle, Listening, Transcribing, Error };

    // Recordings shorter than this are not sent: there is no speech in them, and speech models
    // tend to invent words for near-empty audio.
    static constexpr int kMinRecordingMs = 250;

    struct Callbacks {
        // Every state change, with the reason when the state is Error.
        std::function<void(State, const std::string& error)> on_state;
        // The recognised speech, just before the state returns to Idle.
        std::function<void(std::string text)> on_text;
        // Both can be called on the worker thread. Hand over to the UI thread and return.
    };

    VoiceSession(IAudioRecorder& recorder, ISpeechToText& transcriber, Callbacks callbacks);
    ~VoiceSession();   // cancels: a recording in progress is dropped, never sent

    VoiceSession(const VoiceSession&) = delete;
    VoiceSession& operator=(const VoiceSession&) = delete;

    // Starts recording. False, with nothing changed, while a recording or a transcription is
    // already running; false with state Error when the microphone cannot be started.
    bool start();
    // Ends the recording and sends it for transcription. Does nothing unless Listening.
    void stop();
    // Drops the recording, or abandons the transcription in progress.
    void cancel();

    State state() const;
    // False once the recorder has stopped by itself (length limit or device failure) while the
    // session is still Listening. The owner should then call stop().
    bool capturing() const;

private:
    void set_state(State state, const std::string& error = {});
    void join_worker();
    void transcribe(const std::vector<std::int16_t>& samples, int sample_rate, std::stop_token stop);

    IAudioRecorder& recorder_;
    ISpeechToText& transcriber_;
    Callbacks callbacks_;
    std::thread worker_;
    std::stop_source stop_source_;   // touched only by the owner's thread
    mutable std::mutex mutex_;       // guards state_
    State state_ = State::Idle;
};

}  // namespace buddy
