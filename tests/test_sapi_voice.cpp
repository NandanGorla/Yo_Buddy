// Spoken output. The tests that run by default make no sound: they only cover the paths that
// end before anything is played. The one that plays a (silent) clip is hidden behind a tag,
// because it needs an audio output device: run it with  buddy_tests "[.speech-device]".
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/wav.h"
#include "fake_transport.h"
#include "platform/sapi_voice.h"

using namespace buddy;
using namespace buddy::test;

namespace {

SapiVoice::Config neural_config() {
    SapiVoice::Config config;
    config.neural_base_url = "http://127.0.0.1:8000/v1";
    config.neural_model = "test-voice";
    return config;
}

bool wait_for_call(const FakeTransport& transport) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.calls > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

TEST_CASE("speech: nothing is requested or said while muted", "[speech]") {
    FakeTransport transport;
    SapiVoice voice(&transport);
    voice.configure(neural_config());
    voice.set_muted(true);
    voice.speak("this must stay silent");
    voice.stop();
    CHECK(transport.calls == 0);
}

TEST_CASE("speech: stopping abandons a neural request at once", "[speech]") {
    FakeTransport transport;
    transport.content_type = "audio/wav";
    transport.chunks = {"RIFF"};
    transport.block_after_first_chunk = true;   // the server never finishes
    SapiVoice voice(&transport);
    voice.configure(neural_config());

    voice.speak("hello");
    REQUIRE(wait_for_call(transport));
    CHECK(transport.last_request.url == "http://127.0.0.1:8000/v1/audio/speech");
    CHECK(transport.last_request.body.find("\"input\":\"hello\"") != std::string::npos);
    CHECK(transport.last_request.body.find("\"model\":\"test-voice\"") != std::string::npos);

    // A stopped request is over: there is no fall back to a Windows voice, so this is silent.
    const auto before = std::chrono::steady_clock::now();
    voice.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
}

TEST_CASE("speech: muting silences a request in progress", "[speech]") {
    FakeTransport transport;
    transport.chunks = {"RIFF"};
    transport.block_after_first_chunk = true;
    SapiVoice voice(&transport);
    voice.configure(neural_config());

    voice.speak("hello");
    REQUIRE(wait_for_call(transport));
    voice.set_muted(true);   // returns only once the worker has given up
    voice.speak("still muted");
    CHECK(transport.calls == 1);
}

TEST_CASE("speech: stopping cuts a playing neural clip short", "[.speech-device]") {
    // Five seconds of silence, so the test is inaudible.
    const std::vector<std::int16_t> silence(16'000 * 5, 0);
    const wav::Result clip = wav::encode_pcm16(silence, 16'000, 1);
    REQUIRE(clip.ok());

    FakeTransport transport;
    transport.content_type = "audio/wav";
    transport.chunks = {std::string(clip.bytes.begin(), clip.bytes.end())};
    SapiVoice voice(&transport);
    voice.configure(neural_config());

    // A single space: nothing to pronounce, should the clip fail to play and a Windows voice take over.
    voice.speak(" ");
    REQUIRE(wait_for_call(transport));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // let playback begin

    const auto before = std::chrono::steady_clock::now();
    voice.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
}
