#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "providers/transcription.h"

using namespace buddy;
using namespace buddy::test;

namespace {

LocalTranscriber make(FakeTransport& transport, std::string base = "http://127.0.0.1:8000/v1") {
    return LocalTranscriber(transport, {std::move(base), "whisper-1"});
}

const std::vector<std::uint8_t> kAudio{'R', 'I', 'F', 'F'};

}  // namespace

TEST_CASE("transcription sends multipart WAV data", "[transcription]") {
    FakeTransport transport;
    transport.content_type = "application/json";
    transport.chunks = {R"({"text":"hello world"})"};
    LocalTranscriber transcriber = make(transport);
    const TranscriptionResult result = transcriber.transcribe(kAudio, {});
    REQUIRE(result.ok);
    CHECK(result.text == "hello world");
    CHECK(result.error.empty());
    CHECK(transport.last_request.url == "http://127.0.0.1:8000/v1/audio/transcriptions");
    CHECK(transport.last_request.headers[0].second.find("multipart/form-data; boundary=") == 0);
    CHECK(transport.last_request.body.find("name=\"model\"\r\n\r\nwhisper-1\r\n") != std::string::npos);
    CHECK(transport.last_request.body.find("name=\"file\"; filename=\"audio.wav\"") != std::string::npos);
    CHECK(transport.last_request.body.find("RIFF") != std::string::npos);
}

TEST_CASE("transcription trims the text and treats blank text as no speech", "[transcription]") {
    FakeTransport transport;
    transport.content_type = "application/json";
    // Whisper servers commonly put a space in front of the text.
    transport.chunks = {R"({"text":" hello world \n"})"};
    LocalTranscriber transcriber = make(transport);
    CHECK(transcriber.transcribe(kAudio, {}).text == "hello world");

    transport.chunks = {R"({"text":"   "})"};
    const TranscriptionResult blank = transcriber.transcribe(kAudio, {});
    CHECK_FALSE(blank.ok);
    CHECK(blank.error == "No speech was recognised.");
}

TEST_CASE("transcription rejects malformed and empty responses", "[transcription]") {
    for (const std::string response : {"{}", R"({"text":12})", R"({"text":""})", "not-json", "[]"}) {
        FakeTransport transport;
        transport.content_type = "application/json";
        transport.chunks = {response};
        LocalTranscriber transcriber = make(transport);
        const TranscriptionResult result = transcriber.transcribe(kAudio, {});
        CHECK_FALSE(result.ok);
        CHECK(result.text.empty());
        CHECK_FALSE(result.error.empty());
    }
}

TEST_CASE("transcription reports HTTP errors", "[transcription]") {
    FakeTransport transport;
    transport.status = 404;
    transport.chunks = {R"({"error":"no such model"})"};
    LocalTranscriber transcriber = make(transport);
    const TranscriptionResult result = transcriber.transcribe(kAudio, {});
    CHECK_FALSE(result.ok);
    CHECK(result.http_status == 404);
    CHECK(result.error.find("404") != std::string::npos);
}

TEST_CASE("transcription requires loopback and reports transport failures", "[transcription]") {
    // Recorded speech must never leave this PC: a remote URL is refused before any request.
    FakeTransport remote;
    LocalTranscriber remote_transcriber = make(remote, "https://example.com/v1");
    CHECK_FALSE(remote_transcriber.transcribe(kAudio, {}).ok);
    CHECK(remote.calls == 0);

    FakeTransport failed;
    failed.outcome = HttpOutcome::ConnectFailed;
    LocalTranscriber failed_transcriber = make(failed);
    const TranscriptionResult result = failed_transcriber.transcribe(kAudio, {});
    CHECK_FALSE(result.ok);
    CHECK(result.transport_outcome == HttpOutcome::ConnectFailed);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("transcription refuses to run without audio or a model", "[transcription]") {
    FakeTransport transport;
    LocalTranscriber transcriber = make(transport);
    CHECK_FALSE(transcriber.transcribe({}, {}).ok);

    LocalTranscriber no_model(transport, {"http://127.0.0.1:8000/v1", ""});
    CHECK_FALSE(no_model.transcribe(kAudio, {}).ok);
    CHECK(transport.calls == 0);
}

TEST_CASE("transcription settings can change between requests", "[transcription]") {
    FakeTransport transport;
    transport.content_type = "application/json";
    transport.chunks = {R"({"text":"ok"})"};
    LocalTranscriber transcriber(transport, {});   // nothing configured yet
    CHECK_FALSE(transcriber.transcribe(kAudio, {}).ok);
    CHECK(transport.calls == 0);

    transcriber.configure({"http://localhost:9000/v1", "small.en"});
    REQUIRE(transcriber.transcribe(kAudio, {}).ok);
    CHECK(transport.last_request.url == "http://localhost:9000/v1/audio/transcriptions");
    CHECK(transport.last_request.body.find("small.en") != std::string::npos);
}

TEST_CASE("transcription stops when asked", "[transcription]") {
    FakeTransport transport;
    transport.chunks = {R"({"text":"too late"})"};
    std::stop_source source;
    source.request_stop();
    LocalTranscriber transcriber = make(transport);
    const TranscriptionResult result = transcriber.transcribe(kAudio, source.get_token());
    CHECK_FALSE(result.ok);
    CHECK(result.transport_outcome == HttpOutcome::Cancelled);
}
