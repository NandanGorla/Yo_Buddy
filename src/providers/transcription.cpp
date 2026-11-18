#include "providers/transcription.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "core/text.h"
#include "core/url.h"

namespace buddy {
namespace {

using json = nlohmann::json;
constexpr std::size_t kMaxAudioBytes = 25u * 1024u * 1024u;
constexpr std::size_t kMaxResponseBytes = 64u * 1024u;

std::string transport_error(HttpOutcome outcome) {
    switch (outcome) {
        case HttpOutcome::Cancelled: return "Transcription was cancelled.";
        case HttpOutcome::ConnectFailed:
            return "Could not reach the transcription server. Check that it is running and that its URL in "
                   "Settings is right.";
        case HttpOutcome::Timeout: return "The transcription server took too long to answer.";
        case HttpOutcome::TlsFailed: return "The transcription server's certificate could not be verified.";
        case HttpOutcome::InvalidRequest: return "The transcription URL in Settings is not valid.";
        default: return "The transcription request failed.";
    }
}

TranscriptionResult fail(std::string error, HttpOutcome outcome = HttpOutcome::Completed, int status = 0) {
    return {false, {}, std::move(error), outcome, status};
}

}  // namespace

LocalTranscriber::LocalTranscriber(IHttpTransport& transport, TranscriptionConfig config)
    : transport_(transport), config_(std::move(config)) {}

void LocalTranscriber::configure(TranscriptionConfig config) {
    std::lock_guard lock(mutex_);
    config_ = std::move(config);
}

std::string build_transcription_multipart(std::string_view boundary, std::string_view model,
                                          std::span<const std::uint8_t> wav) {
    std::string body;
    body.reserve(wav.size() + model.size() + boundary.size() * 3 + 256);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    body += model;
    body += "\r\n--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(wav.data()), wav.size());
    body += "\r\n--";
    body += boundary;
    body += "--\r\n";
    return body;
}

TranscriptionResult LocalTranscriber::transcribe(std::span<const std::uint8_t> wav, std::stop_token stop) {
    TranscriptionConfig config;
    {
        std::lock_guard lock(mutex_);
        config = config_;
    }
    if (wav.empty()) return fail("No audio was recorded.");
    if (wav.size() > kMaxAudioBytes) return fail("The recording is too long to transcribe.");
    if (config.model.empty()) return fail("No transcription model is set. Choose one in Settings.");

    const url::BaseUrlResult parsed = url::parse_base_url(config.base_url);
    if (!parsed.ok()) return fail("The transcription URL in Settings is not valid: " + parsed.error);
    if (!url::is_loopback_host(parsed.url->host)) {
        return fail("Voice transcription only works with a server on this PC (a localhost URL).");
    }

    constexpr std::string_view boundary = "----BuddyAudioBoundary7f4e";
    HttpRequest request;
    request.url = parsed.url->to_string() + "/audio/transcriptions";
    request.headers = {{"Content-Type", "multipart/form-data; boundary=" + std::string(boundary)},
                       {"Accept", "application/json"}};
    request.body = build_transcription_multipart(boundary, config.model, wav);

    std::string response_body;
    int status = 0;
    const HttpResponse response = transport_.send(
        request,
        {[&](int code, std::string_view) { status = code; },
         [&](std::string_view chunk) {
             // A transcript is small. Anything past the limit is dropped, and the cut-off JSON
             // then fails to parse below.
             response_body.append(chunk.substr(0, kMaxResponseBytes - response_body.size()));
         }},
        stop);
    if (response.outcome != HttpOutcome::Completed) {
        return fail(transport_error(response.outcome), response.outcome, status);
    }
    if (status < 200 || status >= 300) {
        std::string error = "The transcription server answered with HTTP " + std::to_string(status) + ".";
        if (status == 404) error += " Check the URL and the model name in Settings.";
        return fail(std::move(error), HttpOutcome::Completed, status);
    }

    const json root = json::parse(response_body, nullptr, false);
    const auto text = root.is_object() ? root.find("text") : root.end();
    if (!root.is_object() || text == root.end() || !text->is_string()) {
        return fail("The transcription server sent a reply Buddy could not read.", HttpOutcome::Completed, status);
    }
    std::string value(text::trim_ascii(text->get_ref<const std::string&>()));
    if (value.empty()) return fail("No speech was recognised.", HttpOutcome::Completed, status);
    return {true, std::move(value), {}, HttpOutcome::Completed, status};
}

}  // namespace buddy
