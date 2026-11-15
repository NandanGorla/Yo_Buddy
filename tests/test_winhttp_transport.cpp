// Tests for the real WinHTTP transport against a mock HTTP server on the loopback interface.
// cpp-httplib must be included before any Windows header.
#include <httplib.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "fake_transport.h"
#include "platform/winhttp_transport.h"
#include "providers/openai_compat.h"

using namespace buddy;
using Clock = std::chrono::steady_clock;

namespace {

// A cpp-httplib server on 127.0.0.1 with a port chosen by the OS. Register routes on `server`,
// then call start().
struct MockServer {
    httplib::Server server;
    std::thread thread;
    int port = 0;

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~MockServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

struct Received {
    int status = 0;
    std::string content_type;
    std::string body;
    std::vector<Clock::time_point> chunk_times;
};

HttpHandlers collect(Received& received) {
    HttpHandlers handlers;
    handlers.on_head = [&](int status, std::string_view content_type) {
        received.status = status;
        received.content_type = std::string(content_type);
    };
    handlers.on_chunk = [&](std::string_view chunk) {
        received.body.append(chunk);
        received.chunk_times.push_back(Clock::now());
    };
    return handlers;
}

long long ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

}  // namespace

TEST_CASE("winhttp: a POST reaches the server with its headers and body", "[winhttp]") {
    MockServer mock;
    mock.server.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(req.get_header_value("X-Test") + "|" + req.get_header_value("Content-Type") + "|" + req.body,
                        "text/plain");
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/echo");
    request.headers = {{"Content-Type", "application/json"}, {"X-Test", "hello"}};
    request.body = R"({"a":1})";

    Received received;
    const HttpResponse response = transport.send(request, collect(received), {});
    CHECK(response.outcome == HttpOutcome::Completed);
    CHECK(response.status == 200);
    CHECK(received.status == 200);
    CHECK(received.content_type.find("text/plain") == 0);
    CHECK(received.body == R"(hello|application/json|{"a":1})");
}

TEST_CASE("winhttp: error statuses are reported with their body", "[winhttp]") {
    MockServer mock;
    mock.server.Post("/missing", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(R"({"error":{"message":"no such model"}})", "application/json");
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/missing");
    Received received;
    const HttpResponse response = transport.send(request, collect(received), {});
    CHECK(response.outcome == HttpOutcome::Completed);
    CHECK(response.status == 404);
    CHECK(received.body == R"({"error":{"message":"no such model"}})");
}

TEST_CASE("winhttp: a streamed body arrives piece by piece, not all at the end", "[winhttp]") {
    MockServer mock;
    mock.server.Post("/stream", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("text/event-stream", [](size_t, httplib::DataSink& sink) {
            const std::string first = "data: first\n\n";
            const std::string second = "data: second\n\n";
            sink.write(first.data(), first.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            sink.write(second.data(), second.size());
            sink.done();
            return true;
        });
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/stream");
    Received received;
    const HttpResponse response = transport.send(request, collect(received), {});
    const Clock::time_point finished = Clock::now();

    REQUIRE(response.outcome == HttpOutcome::Completed);
    CHECK(received.content_type.find("text/event-stream") == 0);
    CHECK(received.body == "data: first\n\ndata: second\n\n");
    REQUIRE(received.chunk_times.size() >= 2);
    // The first piece must have been delivered while the server was still sleeping.
    CHECK(ms_between(received.chunk_times.front(), finished) >= 400);
}

TEST_CASE("winhttp: a stop request aborts a stream that is waiting for data", "[winhttp]") {
    std::atomic<bool> release{false};
    MockServer mock;
    mock.server.Post("/hang", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("text/event-stream", [&](size_t, httplib::DataSink& sink) {
            const std::string first = "data: first\n\n";
            sink.write(first.data(), first.size());
            // Then say nothing, like a model that has stalled.
            for (int i = 0; i < 100 && !release.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            sink.done();
            return true;
        });
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/hang");

    std::stop_source stop;
    std::atomic<bool> got_first{false};
    Received received;
    HttpHandlers handlers = collect(received);
    const auto inner = handlers.on_chunk;
    handlers.on_chunk = [&](std::string_view chunk) {
        inner(chunk);
        got_first = true;
    };

    // Another thread plays the user pressing Stop shortly after the first text arrived.
    std::thread stopper([&] {
        while (!got_first.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.request_stop();
    });

    const Clock::time_point started = Clock::now();
    const HttpResponse response = transport.send(request, handlers, stop.get_token());
    const long long elapsed = ms_between(started, Clock::now());
    stopper.join();
    release = true;   // let the server handler finish so the mock can shut down

    CHECK(response.outcome == HttpOutcome::Cancelled);
    CHECK(received.body == "data: first\n\n");
    CHECK(elapsed < 3000);   // far sooner than the 10 seconds the server would have waited
}

TEST_CASE("winhttp: a request that is already cancelled is never sent", "[winhttp]") {
    std::atomic<int> hits{0};
    MockServer mock;
    mock.server.Post("/x", [&](const httplib::Request&, httplib::Response& res) {
        ++hits;
        res.set_content("ok", "text/plain");
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/x");
    std::stop_source stop;
    stop.request_stop();
    Received received;
    CHECK(transport.send(request, collect(received), stop.get_token()).outcome == HttpOutcome::Cancelled);
    CHECK(hits == 0);
}

TEST_CASE("winhttp: nothing listening is a connect failure", "[winhttp]") {
    int free_port = 0;
    {
        MockServer mock;
        mock.start();
        free_port = mock.port;
    }   // the server is gone; its port is now closed

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(free_port) + "/v1/chat/completions";
    request.connect_timeout_ms = 4000;
    Received received;
    const HttpResponse response = transport.send(request, collect(received), {});
    CHECK(response.outcome == HttpOutcome::ConnectFailed);
    CHECK(received.status == 0);
}

TEST_CASE("winhttp: redirects are not followed", "[winhttp]") {
    std::atomic<int> target_hits{0};
    MockServer mock;
    mock.server.Post("/redirect", [](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", "/target");
    });
    const auto count_hit = [&](const httplib::Request&, httplib::Response& res) {
        ++target_hits;
        res.set_content("should not be reached", "text/plain");
    };
    mock.server.Post("/target", count_hit);
    mock.server.Get("/target", count_hit);
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    HttpRequest request;
    request.url = mock.url("/redirect");
    request.headers = {{"Authorization", "Bearer secret"}};
    Received received;
    const HttpResponse response = transport.send(request, collect(received), {});
    CHECK(response.outcome == HttpOutcome::Completed);
    CHECK(response.status == 302);
    CHECK(target_hits == 0);
}

TEST_CASE("winhttp: header injection and bad URLs are refused before any connection", "[winhttp]") {
    WinHttpTransport transport(L"BuddyTests/1.0");
    Received received;

    HttpRequest injected;
    injected.url = "http://127.0.0.1:9/x";
    injected.headers = {{"Authorization", "Bearer abc\r\nX-Injected: 1"}};
    CHECK(transport.send(injected, collect(received), {}).outcome == HttpOutcome::InvalidRequest);

    HttpRequest bad_name;
    bad_name.url = "http://127.0.0.1:9/x";
    bad_name.headers = {{"Bad:Name", "v"}};
    CHECK(transport.send(bad_name, collect(received), {}).outcome == HttpOutcome::InvalidRequest);

    HttpRequest bad_url;
    bad_url.url = "not a url";
    CHECK(transport.send(bad_url, collect(received), {}).outcome == HttpOutcome::InvalidRequest);

    HttpRequest wrong_scheme;
    wrong_scheme.url = "ftp://127.0.0.1/x";
    CHECK(transport.send(wrong_scheme, collect(received), {}).outcome == HttpOutcome::InvalidRequest);
}

TEST_CASE("winhttp + provider: a streamed chat against a mock chat completions server", "[winhttp][provider]") {
    std::string seen_auth;
    std::string seen_body;
    MockServer mock;
    mock.server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        seen_auth = req.get_header_value("Authorization");
        seen_body = req.body;
        if (seen_auth != "Bearer good-key") {
            res.status = 401;
            res.set_content(R"({"error":{"message":"Incorrect API key provided","type":"invalid_request_error"}})",
                            "application/json");
            return;
        }
        res.set_chunked_content_provider("text/event-stream", [](size_t, httplib::DataSink& sink) {
            for (const std::string& piece :
                 {test::text_chunk("Hello"), test::text_chunk(", "), test::text_chunk("world"),
                  test::finish_chunk("stop"), test::usage_chunk(9, 3), test::done_marker()}) {
                sink.write(piece.data(), piece.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            sink.done();
            return true;
        });
    });
    mock.start();

    WinHttpTransport transport(L"BuddyTests/1.0");
    ChatOptions options;
    options.model = "mock-model";
    options.max_output_tokens = 256;

    SECTION("with the right key the reply streams through") {
        OpenAiCompatProvider provider(transport, {mock.url("/v1"), "good-key"});
        std::string text;
        int pieces = 0;
        ChatSink sink;
        sink.on_text = [&](std::string_view piece) {
            text.append(piece);
            ++pieces;
        };
        const ChatResult result = provider.chat({{Role::User, "hi"}}, {}, {}, options, sink, {});
        CHECK(result.status == ChatStatus::Ok);
        CHECK(text == "Hello, world");
        CHECK(pieces == 3);
        CHECK(result.input_tokens == 9);
        CHECK(result.output_tokens == 3);
        const auto body = nlohmann::json::parse(seen_body);
        CHECK(body["model"] == "mock-model");
        CHECK(body["stream"] == true);
    }

    SECTION("with a wrong key the server's 401 becomes an auth failure") {
        OpenAiCompatProvider provider(transport, {mock.url("/v1"), "bad-key"});
        std::string text;
        ChatSink sink;
        sink.on_text = [&](std::string_view piece) { text.append(piece); };
        const ChatResult result = provider.chat({{Role::User, "hi"}}, {}, {}, options, sink, {});
        CHECK(result.status == ChatStatus::AuthFailed);
        CHECK(result.http_status == 401);
        CHECK(result.message == "Incorrect API key provided");
        CHECK(text.empty());
        CHECK(seen_auth == "Bearer bad-key");
    }
}
