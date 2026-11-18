#include "app/voice_session.h"

#include <cstddef>
#include <utility>

#include "core/wav.h"

namespace buddy {

VoiceSession::VoiceSession(IAudioRecorder& recorder, ISpeechToText& transcriber, Callbacks callbacks)
    : recorder_(recorder), transcriber_(transcriber), callbacks_(std::move(callbacks)) {}

VoiceSession::~VoiceSession() {
    cancel();
    join_worker();
}

void VoiceSession::set_state(State state, const std::string& error) {
    {
        std::lock_guard lock(mutex_);
        state_ = state;
    }
    // Outside the lock, so a callback may ask for the state.
    if (callbacks_.on_state) callbacks_.on_state(state, error);
}

void VoiceSession::join_worker() {
    if (worker_.joinable()) worker_.join();
}

bool VoiceSession::start() {
    const State now = state();
    if (now == State::Listening || now == State::Transcribing) return false;
    // Idle or Error: an earlier worker has reported its result and is at most returning.
    join_worker();

    if (!recorder_.start()) {
        const std::string error = recorder_.error();
        set_state(State::Error, error.empty() ? "The microphone could not be started." : error);
        return false;
    }
    set_state(State::Listening);
    return true;
}

void VoiceSession::stop() {
    if (state() != State::Listening) return;

    std::vector<std::int16_t> samples = recorder_.stop();
    const int sample_rate = recorder_.sample_rate();
    // The microphone is opened on the recorder's own thread, so a device problem only shows now.
    if (const std::string error = recorder_.error(); !error.empty()) {
        set_state(State::Error, error);
        return;
    }
    if (samples.empty()) {
        set_state(State::Error, "No sound came from the microphone.");
        return;
    }
    const auto min_samples = static_cast<std::size_t>(sample_rate) * kMinRecordingMs / 1000;
    if (samples.size() < min_samples) {
        set_state(State::Error, "That recording was too short to transcribe.");
        return;
    }

    join_worker();
    stop_source_ = std::stop_source();
    set_state(State::Transcribing);
    worker_ = std::thread([this, samples = std::move(samples), sample_rate, stop = stop_source_.get_token()] {
        transcribe(samples, sample_rate, stop);
    });
}

void VoiceSession::cancel() {
    switch (state()) {
        case State::Listening:
            recorder_.stop();   // the recording is discarded
            set_state(State::Idle);
            break;
        case State::Transcribing:
            stop_source_.request_stop();
            join_worker();      // the worker reports Idle on its way out
            break;
        case State::Idle:
        case State::Error:
            break;
    }
}

VoiceSession::State VoiceSession::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool VoiceSession::capturing() const {
    return recorder_.recording();
}

void VoiceSession::transcribe(const std::vector<std::int16_t>& samples, int sample_rate, std::stop_token stop) {
    const wav::Result wav = wav::encode_pcm16(samples, sample_rate, 1);
    if (!wav.ok()) {
        set_state(State::Error, "The recording could not be prepared for transcription.");
        return;
    }
    const TranscriptionResult result = transcriber_.transcribe(wav.bytes, stop);
    if (stop.stop_requested() || result.transport_outcome == HttpOutcome::Cancelled) {
        set_state(State::Idle);
        return;
    }
    if (!result.ok) {
        set_state(State::Error, result.error.empty() ? "Voice transcription failed." : result.error);
        return;
    }
    if (callbacks_.on_text) callbacks_.on_text(result.text);
    set_state(State::Idle);
}

}  // namespace buddy
