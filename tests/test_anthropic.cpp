#include <catch2/catch_test_macros.hpp>

#include "fake_transport.h"
#include "providers/anthropic.h"

using namespace buddy;
using namespace buddy::test;
using json = nlohmann::json;

TEST_CASE("Anthropic request uses native Messages format", "[anthropic]") {
    ChatOptions options;
    options.model = "claude-test";
    options.max_output_tokens = 128;
    const json body = build_anthropic_request(
        {{Role::System, "be concise"}, {Role::User, "look"}}, {{"image/png", "QUJD"}}, {}, options);

    CHECK(body["model"] == "claude-test");
    CHECK(body["max_tokens"] == 128);
    CHECK(body["stream"] == true);
    CHECK(body["system"] == "be concise");
    CHECK(body["messages"][0]["role"] == "user");
    CHECK(body["messages"][0]["content"][1]["type"] == "image");
    CHECK(body["messages"][0]["content"][1]["source"]["type"] == "base64");
    CHECK_FALSE(body.contains("temperature"));
}

TEST_CASE("Anthropic streaming yields text and usage", "[anthropic]") {
    FakeTransport transport;
    transport.chunks = {
        sse(R"({"type":"message_start","message":{"usage":{"input_tokens":11}}})"),
        sse(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
        sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"),
        sse(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":4}})"),
        sse(R"({"type":"message_stop"})")};

    AnthropicProvider provider(transport, {"https://api.anthropic.com/v1", "sk-test", "2023-06-01"});
    ChatOptions options;
    options.model = "claude-test";
    std::string text;
    ChatSink sink;
    sink.on_text = [&](std::string_view piece) { text += piece; };
    const ChatResult result = provider.chat({{Role::User, "hi"}}, {}, {}, options, sink, {});

    CHECK(result.status == ChatStatus::Ok);
    CHECK(text == "Hello");
    CHECK(result.stop_reason == "end_turn");
    CHECK(result.input_tokens == 11);
    CHECK(result.output_tokens == 4);
    CHECK(transport.last_request.url == "https://api.anthropic.com/v1/messages");
    CHECK(transport.last_request.body.find("\"messages\"") != std::string::npos);
}

TEST_CASE("Anthropic errors expose safe API messages", "[anthropic]") {
    FakeTransport transport;
    transport.status = 429;
    transport.content_type = "application/json";
    transport.chunks = {R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})"};
    AnthropicProvider provider(transport, {"https://api.anthropic.com/v1", "sk-test", "2023-06-01"});
    ChatOptions options;
    options.model = "claude-test";
    const ChatResult result = provider.chat({{Role::User, "hi"}}, {}, {}, options, {}, {});
    CHECK(result.status == ChatStatus::RateLimited);
    CHECK(result.message == "slow down");
}
