#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "providers/http.h"

namespace buddy::test {

// A scripted IHttpTransport: the "mock backend" for provider and controller tests.
// It records the request it was given and replays a canned response.
class FakeTransport final : public IHttpTransport {
public:
    // What to answer.
    int status = 200;
    std::string content_type = "text/event-stream";
    std::vector<std::string> chunks;
    HttpOutcome outcome = HttpOutcome::Completed;   // anything but Completed fails before the head
    bool block_after_first_chunk = false;           // then wait until the stop token fires

    // What was asked.
    HttpRequest last_request;
    int calls = 0;

    HttpResponse send(const HttpRequest& request, const HttpHandlers& handlers, std::stop_token stop) override {
        last_request = request;
        ++calls;

        HttpResponse response;
        if (outcome != HttpOutcome::Completed) {
            response.outcome = outcome;
            return response;
        }
        response.status = status;
        if (handlers.on_head) handlers.on_head(status, content_type);

        for (std::size_t i = 0; i < chunks.size(); ++i) {
            if (stop.stop_requested()) {
                response.outcome = HttpOutcome::Cancelled;
                return response;
            }
            if (handlers.on_chunk) handlers.on_chunk(chunks[i]);
            if (i == 0 && block_after_first_chunk) {
                while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                response.outcome = HttpOutcome::Cancelled;
                return response;
            }
        }
        response.outcome = HttpOutcome::Completed;
        return response;
    }
};

// One server-sent event carrying `data`.
inline std::string sse(const std::string& data) { return "data: " + data + "\n\n"; }

// A streamed chat chunk holding one piece of reply text.
inline std::string text_chunk(const std::string& json_escaped_text) {
    return sse(R"({"id":"c1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":")" +
               json_escaped_text + R"("},"finish_reason":null}]})");
}

inline std::string text_chunk_raw(const std::string& text) {
    const std::string encoded = nlohmann::json(text).dump();
    return text_chunk(encoded.substr(1, encoded.size() - 2));
}

inline std::string finish_chunk(const std::string& reason) {
    return sse(R"({"id":"c1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":")" +
               reason + R"("}]})");
}

inline std::string usage_chunk(int prompt_tokens, int completion_tokens) {
    return sse(R"({"id":"c1","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":)" +
               std::to_string(prompt_tokens) + R"(,"completion_tokens":)" + std::to_string(completion_tokens) +
               R"(,"total_tokens":)" + std::to_string(prompt_tokens + completion_tokens) + "}}");
}

inline std::string done_marker() { return sse("[DONE]"); }

}  // namespace buddy::test
