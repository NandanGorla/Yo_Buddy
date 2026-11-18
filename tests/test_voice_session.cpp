// The push-to-talk round (record, transcribe, hand over the text) against a fake microphone and
// a fake speech-to-text backend, so no sound device and no server are needed.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/voice_session.h"

using namespace buddy;
using State = VoiceSession::State;

namespace {

class FakeRecorder final : public IAudioRecorder {
public:
    bool can_start = true;
    std::string start_error = "No microphone was found.";
    std::string failure;                  // reported after stop(), like a device that went away
    std::vector<std::int16_t> recorded;   // what stop() returns
    int rate = 16'000;
    int starts = 0;
    int stops = 0;

    bool start() override {
        ++starts;
        error_.clear();
        if (!can_start) {
            error_ = start_error;
            return false;
        }
        recording_ = true;
        return true;
    }
    std::vector<std::int16_t> stop() override {
        ++stops;
        recording_ = false;
        error_ = failure;
        return recorded;
    }
    bool recording() const override { return recording_; }
    std::string error() const override { return error_; }
    int sample_rate() const override { return rate; }

    // One second of audio: comfortably longer than the minimum.
    void record_one_second() { recorded.assign(static_cast<std::size_t>(rate), std::int16_t{100}); }

private:
    std::atomic<bool> recording_{false};
    std::string error_;
};

class FakeTranscriber final : public ISpeechToText {
public:
    TranscriptionResult result{true, "hello buddy", {}, HttpOutcome::Completed, 200};
    std::atomic<bool> hold{false};   // keep the request "in flight" until released or stopped
    std::atomic<int> calls{0};
    std::atomic<bool> saw_stop{false};
    std::vector<std::uint8_t> last_wav;

    TranscriptionResult transcribe(std::span<const std::uint8_t> wav, std::stop_token stop) override {
        last_wav.assign(wav.begin(), wav.end());
        ++calls;
        while (hold.load() && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested()) {
            saw_stop = true;
            return {false, {}, "Transcription was cancelled.", HttpOutcome::Cancelled, 0};
        }
        return result;
    }
};

struct Rig {
    FakeRecorder recorder;
    FakeTranscriber transcriber;

    std::mutex mutex;
    std::vector<std::pair<State, std::string>> states;
    std::vector<std::string> texts;

    VoiceSession session{recorder, transcriber,
                         VoiceSession::Callbacks{[this](State state, const std::string& error) {
                                                     std::lock_guard lock(mutex);
                                                     states.emplace_back(state, error);
                                                 },
                                                 [this](std::string text) {
                                                     std::lock_guard lock(mutex);
                                                     texts.push_back(std::move(text));
                                                 }}};

    std::vector<State> state_sequence() {
        std::lock_guard lock(mutex);
        std::vector<State> out;
        for (const auto& entry : states) out.push_back(entry.first);
        return out;
    }
    std::string last_error() {
        std::lock_guard lock(mutex);
        return states.empty() ? std::string() : states.back().second;
    }
    std::vector<std::string> heard() {
        std::lock_guard lock(mutex);
        return texts;
    }
};

bool wait_until(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

TEST_CASE("voice: a recording is transcribed and the text handed over", "[voice]") {
    Rig rig;
    rig.recorder.rate = 48'000;
    rig.recorder.record_one_second();

    REQUIRE(rig.session.start());
    CHECK(rig.session.state() == State::Listening);
    CHECK(rig.session.capturing());

    rig.session.stop();
    REQUIRE(wait_until([&] { return rig.session.state() == State::Idle; }));

    CHECK(rig.heard() == std::vector<std::string>{"hello buddy"});
    CHECK(rig.state_sequence() == std::vector<State>{State::Listening, State::Transcribing, State::Idle});

    // The backend got one mono 16-bit WAV file at the microphone's own sample rate.
    REQUIRE(rig.transcriber.calls == 1);
    const auto& wav = rig.transcriber.last_wav;
    REQUIRE(wav.size() == 44 + 48'000 * 2);
    CHECK(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
    CHECK(wav[22] == 1);                   // channels
    CHECK(u32_at(wav, 24) == 48'000);      // sample rate
}

TEST_CASE("voice: a microphone that cannot start is an error, with its reason", "[voice]") {
    Rig rig;
    rig.recorder.can_start = false;

    CHECK_FALSE(rig.session.start());
    CHECK(rig.session.state() == State::Error);
    CHECK(rig.last_error() == "No microphone was found.");
    CHECK(rig.transcriber.calls == 0);

    // The error does not stick: the next attempt can work.
    rig.recorder.can_start = true;
    CHECK(rig.session.start());
    CHECK(rig.session.state() == State::Listening);
}

TEST_CASE("voice: a microphone that failed while recording reports its own reason", "[voice]") {
    Rig rig;
    REQUIRE(rig.session.start());
    rig.recorder.failure = "The microphone was disconnected.";
    rig.recorder.record_one_second();

    rig.session.stop();
    CHECK(rig.session.state() == State::Error);
    CHECK(rig.last_error() == "The microphone was disconnected.");
    CHECK(rig.transcriber.calls == 0);   // a broken recording is not sent
}

TEST_CASE("voice: empty and very short recordings are not sent", "[voice]") {
    SECTION("nothing recorded") {
        Rig rig;
        REQUIRE(rig.session.start());
        rig.session.stop();
        CHECK(rig.session.state() == State::Error);
        CHECK_FALSE(rig.last_error().empty());
        CHECK(rig.transcriber.calls == 0);
    }
    SECTION("a fraction of a second") {
        Rig rig;
        REQUIRE(rig.session.start());
        rig.recorder.recorded.assign(1'600, std::int16_t{100});   // 100 ms at 16 kHz
        rig.session.stop();
        CHECK(rig.session.state() == State::Error);
        CHECK_FALSE(rig.last_error().empty());
        CHECK(rig.transcriber.calls == 0);
    }
}

TEST_CASE("voice: the shortcut during a transcription neither waits nor starts a recording", "[voice]") {
    Rig rig;
    rig.recorder.record_one_second();
    rig.transcriber.hold = true;

    REQUIRE(rig.session.start());
    rig.session.stop();
    REQUIRE(wait_until([&] { return rig.transcriber.calls == 1; }));
    REQUIRE(rig.session.state() == State::Transcribing);

    // This is called on the UI thread: it must come straight back, not wait for the server.
    const auto before = std::chrono::steady_clock::now();
    CHECK_FALSE(rig.session.start());
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK(rig.session.state() == State::Transcribing);
    CHECK(rig.recorder.starts == 1);

    rig.transcriber.hold = false;
    REQUIRE(wait_until([&] { return rig.session.state() == State::Idle; }));
    CHECK(rig.heard() == std::vector<std::string>{"hello buddy"});
}

TEST_CASE("voice: cancelling a recording drops it", "[voice]") {
    Rig rig;
    rig.recorder.record_one_second();
    REQUIRE(rig.session.start());

    rig.session.cancel();
    CHECK(rig.session.state() == State::Idle);
    CHECK(rig.recorder.stops == 1);
    CHECK(rig.transcriber.calls == 0);
    CHECK(rig.heard().empty());
    CHECK(rig.state_sequence() == std::vector<State>{State::Listening, State::Idle});
}

TEST_CASE("voice: cancelling a transcription abandons the request and delivers no text", "[voice]") {
    Rig rig;
    rig.recorder.record_one_second();
    rig.transcriber.hold = true;
    REQUIRE(rig.session.start());
    rig.session.stop();
    REQUIRE(wait_until([&] { return rig.transcriber.calls == 1; }));

    rig.session.cancel();   // waits for the worker, which stops at once
    CHECK(rig.transcriber.saw_stop);
    CHECK(rig.session.state() == State::Idle);
    CHECK(rig.heard().empty());
    CHECK(rig.state_sequence() == std::vector<State>{State::Listening, State::Transcribing, State::Idle});
}

TEST_CASE("voice: destroying the session mid-recording sends nothing", "[voice]") {
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    recorder.record_one_second();
    {
        VoiceSession session(recorder, transcriber, {});
        REQUIRE(session.start());
    }
    CHECK(recorder.stops == 1);       // the microphone was released
    CHECK(transcriber.calls == 0);    // and what it had recorded went nowhere
}

TEST_CASE("voice: destroying the session mid-transcription does not hang", "[voice]") {
    FakeRecorder recorder;
    FakeTranscriber transcriber;
    recorder.record_one_second();
    transcriber.hold = true;
    {
        VoiceSession session(recorder, transcriber, {});
        REQUIRE(session.start());
        session.stop();
        REQUIRE(wait_until([&] { return transcriber.calls == 1; }));
    }
    CHECK(transcriber.saw_stop);
}

TEST_CASE("voice: a failed transcription is an error and the next round still works", "[voice]") {
    Rig rig;
    rig.recorder.record_one_second();
    rig.transcriber.result = {false, {}, "Could not reach the transcription server.", HttpOutcome::ConnectFailed, 0};

    REQUIRE(rig.session.start());
    rig.session.stop();
    REQUIRE(wait_until([&] { return rig.session.state() == State::Error; }));
    CHECK(rig.last_error() == "Could not reach the transcription server.");
    CHECK(rig.heard().empty());

    rig.transcriber.result = {true, "second try", {}, HttpOutcome::Completed, 200};
    REQUIRE(rig.session.start());
    rig.session.stop();
    REQUIRE(wait_until([&] { return rig.session.state() == State::Idle; }));
    CHECK(rig.heard() == std::vector<std::string>{"second try"});
}
