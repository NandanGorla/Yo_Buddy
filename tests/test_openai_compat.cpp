#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "fake_transport.h"
#include "providers/openai_compat.h"

using namespace buddy;
using namespace buddy::test;
using json = nlohmann::json;

namespace {

struct Run {
    ChatResult result;
    std::string text;
};

Run run_chat(FakeTransport& transport, std::string api_key = "sk-test", std::string model = "test-model") {
    OpenAiCompatProvider provider(transport, {"https://api.example.com/v1", std::move(api_key)});
    Run run;
    ChatSink sink;
    sink.on_text = [&](std::string_view piece) { run.text.append(piece); };
    ChatOptions options;
    options.model = std::move(model);
    options.max_output_tokens = 1000;
    run.result = provider.chat({{Role::System, "be brief"}, {Role::User, "hi"}}, {}, {}, options, sink, {});
    return run;
}

std::string header(const HttpRequest& request, const std::string& name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) return value;
    }
    return "<absent>";
}

}  // namespace

TEST_CASE("request body uses the chat completions format", "[provider]") {
    ChatOptions options;
    options.model = "m1";
    options.max_output_tokens = 512;
    const json body = build_chat_request(
        {{Role::System, "sys"}, {Role::User, "first"}, {Role::Assistant, "reply"}, {Role::User, "second"}}, {}, {},
        options);

    CHECK(body["model"] == "m1");
    CHECK(body["stream"] == true);
    CHECK(body["stream_options"]["include_usage"] == true);
    CHECK(body["max_completion_tokens"] == 512);
    REQUIRE(body["messages"].size() == 4);
    CHECK(body["messages"][0] == json{{"role", "system"}, {"content", "sys"}});
    CHECK(body["messages"][2] == json{{"role", "assistant"}, {"content", "reply"}});
    CHECK(body["messages"][3]["content"] == "second");
    // No sampling parameters: current Claude models reject non-default values.
    CHECK_FALSE(body.contains("temperature"));
    CHECK_FALSE(body.contains("top_p"));
    CHECK_FALSE(body.contains("tools"));
}

TEST_CASE("max_completion_tokens is omitted when no limit is set", "[provider]") {
    ChatOptions options;
    options.model = "m1";
    const json body = build_chat_request({{Role::User, "x"}}, {}, {}, options);
    CHECK_FALSE(body.contains("max_completion_tokens"));
}

TEST_CASE("images attach to the newest user message as data URLs", "[provider]") {
    ChatOptions options;
    options.model = "m1";
    const json body = build_chat_request({{Role::User, "old"}, {Role::Assistant, "a"}, {Role::User, "look"}},
                                         {{"image/png", "QUJD"}}, {}, options);
    CHECK(body["messages"][0]["content"] == "old");
    const json& parts = body["messages"][2]["content"];
    REQUIRE(parts.is_array());
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == json{{"type", "text"}, {"text", "look"}});
    CHECK(parts[1]["type"] == "image_url");
    CHECK(parts[1]["image_url"]["url"] == "data:image/png;base64,QUJD");
}

TEST_CASE("tools are sent as function definitions", "[provider]") {
    ChatOptions options;
    options.model = "m1";
    ToolSpec tool;
    tool.name = "draw_arrow";
    tool.description = "Draw an arrow";
    tool.parameters = {{"type", "object"}, {"properties", {{"element_id", {{"type", "integer"}}}}}};
    const json body = build_chat_request({{Role::User, "x"}}, {}, {tool}, options);
    REQUIRE(body["tools"].size() == 1);
    CHECK(body["tools"][0]["type"] == "function");
    CHECK(body["tools"][0]["function"]["name"] == "draw_arrow");
    CHECK(body["tools"][0]["function"]["parameters"]["properties"]["element_id"]["type"] == "integer");
}

TEST_CASE("a streamed reply is delivered piece by piece", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("Hel"), text_chunk("lo"), finish_chunk("stop"), usage_chunk(12, 3), done_marker()};
    const Run run = run_chat(transport);

    CHECK(run.result.status == ChatStatus::Ok);
    CHECK(run.text == "Hello");
    CHECK(run.result.stop_reason == "stop");
    CHECK(run.result.input_tokens == 12);
    CHECK(run.result.output_tokens == 3);

    CHECK(transport.last_request.method == "POST");
    CHECK(transport.last_request.url == "https://api.example.com/v1/chat/completions");
    CHECK(header(transport.last_request, "Authorization") == "Bearer sk-test");
    CHECK(header(transport.last_request, "Content-Type") == "application/json");
    const json body = json::parse(transport.last_request.body);
    CHECK(body["model"] == "test-model");
    CHECK(body["messages"][1]["content"] == "hi");
}

TEST_CASE("network chunks that split events anywhere give the same reply", "[provider]") {
    const std::string stream = text_chunk("caf\\u00e9 ") + text_chunk("\\ud83d\\ude00") + finish_chunk("stop") + done_marker();
    FakeTransport transport;
    for (const char c : stream) transport.chunks.emplace_back(1, c);   // one byte per chunk
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::Ok);
    CHECK(run.text == "caf\xC3\xA9 \xF0\x9F\x98\x80");
}

TEST_CASE("no Authorization header is sent without a key", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("ok"), finish_chunk("stop"), done_marker()};
    const Run run = run_chat(transport, "");
    CHECK(run.result.status == ChatStatus::Ok);
    CHECK(header(transport.last_request, "Authorization") == "<absent>");
}

TEST_CASE("hostile reply text is passed through untouched for the UI to sanitise", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("<script>alert(1)</script><img src=x onerror=alert(2)>"), finish_chunk("stop"),
                        done_marker()};
    const Run run = run_chat(transport);
    CHECK(run.text == "<script>alert(1)</script><img src=x onerror=alert(2)>");
}

TEST_CASE("a reply cut off by the length limit still succeeds", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("partial"), finish_chunk("length"), done_marker()};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::Ok);
    CHECK(run.result.stop_reason == "length");
}

TEST_CASE("a finish reason without the DONE marker is accepted", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("x"), finish_chunk("stop")};
    CHECK(run_chat(transport).result.status == ChatStatus::Ok);
}

TEST_CASE("a stream that ends early is reported, not treated as complete", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("half a rep")};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::BadResponse);
    CHECK(run.text == "half a rep");
}

TEST_CASE("an empty stream is a bad response", "[provider]") {
    FakeTransport transport;
    CHECK(run_chat(transport).result.status == ChatStatus::BadResponse);
}

TEST_CASE("tool call fragments are assembled", "[provider]") {
    FakeTransport transport;
    transport.chunks = {
        sse(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"draw_arrow","arguments":""}}]}}]})"),
        sse(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"element"}}]}}]})"),
        sse(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"_id\":7}"}}]}}]})"),
        finish_chunk("tool_calls"), done_marker()};
    const Run run = run_chat(transport);
    REQUIRE(run.result.status == ChatStatus::Ok);
    REQUIRE(run.result.tool_calls.size() == 1);
    CHECK(run.result.tool_calls[0].id == "call_1");
    CHECK(run.result.tool_calls[0].name == "draw_arrow");
    CHECK(run.result.tool_calls[0].arguments == R"({"element_id":7})");
    CHECK(run.result.stop_reason == "tool_calls");
}

TEST_CASE("HTTP errors map to statuses with the server's message", "[provider]") {
    struct Case {
        int http;
        ChatStatus expected;
    };
    for (const Case c : {Case{401, ChatStatus::AuthFailed}, Case{403, ChatStatus::AuthFailed},
                         Case{429, ChatStatus::RateLimited}, Case{400, ChatStatus::BadRequest},
                         Case{404, ChatStatus::BadRequest}, Case{500, ChatStatus::ServerError},
                         Case{529, ChatStatus::ServerError}, Case{302, ChatStatus::BadResponse}}) {
        FakeTransport transport;
        transport.status = c.http;
        transport.content_type = "application/json";
        transport.chunks = {R"({"error":{"message":"the server says no","type":"some_error"}})"};
        const Run run = run_chat(transport);
        INFO("HTTP " << c.http);
        CHECK(run.result.status == c.expected);
        CHECK(run.result.message == "the server says no");
        CHECK(run.result.http_status == c.http);
        CHECK(run.text.empty());
    }
}

TEST_CASE("Anthropic's native error shape is understood", "[provider]") {
    FakeTransport transport;
    transport.status = 401;
    transport.content_type = "application/json";
    transport.chunks = {R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::AuthFailed);
    CHECK(run.result.message == "invalid x-api-key");
}

TEST_CASE("an HTML error page is never shown to the user", "[provider]") {
    FakeTransport transport;
    transport.status = 502;
    transport.content_type = "text/html";
    transport.chunks = {"<html><body><script>alert(1)</script>Bad gateway</body></html>"};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::ServerError);
    CHECK(run.result.message.find('<') == std::string::npos);
    CHECK(run.result.message.find("502") != std::string::npos);
}

TEST_CASE("a 200 that is not a chat reply is a bad response", "[provider]") {
    FakeTransport transport;
    transport.content_type = "text/html";
    transport.chunks = {"<html>captive portal</html>"};
    CHECK(run_chat(transport).result.status == ChatStatus::BadResponse);
}

TEST_CASE("a server that ignores streaming still yields the reply", "[provider]") {
    FakeTransport transport;
    transport.content_type = "application/json";
    transport.chunks = {R"({"choices":[{"index":0,"message":{"role":"assistant","content":"whole reply"},"finish_reason":"stop"}]})"};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::Ok);
    CHECK(run.text == "whole reply");
}

TEST_CASE("an error event in the middle of a stream fails the request", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("so far"), sse(R"({"error":{"message":"overloaded","type":"overloaded_error"}})")};
    const Run run = run_chat(transport);
    CHECK(run.result.status == ChatStatus::ServerError);
    CHECK(run.result.message == "overloaded");
}

TEST_CASE("transport failures map to statuses", "[provider]") {
    struct Case {
        HttpOutcome outcome;
        ChatStatus expected;
    };
    for (const Case c : {Case{HttpOutcome::ConnectFailed, ChatStatus::NetworkError},
                         Case{HttpOutcome::TlsFailed, ChatStatus::NetworkError},
                         Case{HttpOutcome::Failed, ChatStatus::NetworkError},
                         Case{HttpOutcome::Timeout, ChatStatus::Timeout},
                         Case{HttpOutcome::InvalidRequest, ChatStatus::BadRequest},
                         Case{HttpOutcome::Cancelled, ChatStatus::Cancelled}}) {
        FakeTransport transport;
        transport.outcome = c.outcome;
        const Run run = run_chat(transport);
        CHECK(run.result.status == c.expected);
        if (c.expected != ChatStatus::Cancelled) CHECK_FALSE(run.result.message.empty());
    }
}

TEST_CASE("no request is made without a model", "[provider]") {
    FakeTransport transport;
    const Run run = run_chat(transport, "sk-test", "");
    CHECK(run.result.status == ChatStatus::NotConfigured);
    CHECK(transport.calls == 0);
}

TEST_CASE("cancelling mid-stream keeps the text received so far", "[provider]") {
    FakeTransport transport;
    transport.chunks = {text_chunk("first"), text_chunk("never sent")};
    transport.block_after_first_chunk = true;

    OpenAiCompatProvider provider(transport, {"https://api.example.com/v1", "k"});
    std::string text;
    std::stop_source stop;
    ChatSink sink;
    sink.on_text = [&](std::string_view piece) {
        text.append(piece);
        stop.request_stop();   // the user pressed Stop as soon as text appeared
    };
    ChatOptions options;
    options.model = "m";
    const ChatResult result = provider.chat({{Role::User, "hi"}}, {}, {}, options, sink, stop.get_token());
    CHECK(result.status == ChatStatus::Cancelled);
    CHECK(result.stop_reason == "cancelled");
    CHECK(text == "first");
}

TEST_CASE("status codes have stable names for the UI", "[provider]") {
    CHECK(std::string(to_code(ChatStatus::AuthFailed)) == "auth_failed");
    CHECK(std::string(to_code(ChatStatus::NotConfigured)) == "not_configured");
    CHECK(std::string(to_code(ChatStatus::Cancelled)) == "cancelled");
}
