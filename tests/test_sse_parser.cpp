#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "providers/sse_parser.h"

using buddy::SseParser;

namespace {

using Events = std::vector<std::pair<std::string, std::string>>;

Events parse_whole(const std::string& input) {
    SseParser parser;
    Events events;
    parser.feed(input, [&](std::string_view e, std::string_view d) { events.emplace_back(e, d); });
    return events;
}

// Feeds the input one byte at a time: the hardest case for an incremental parser.
Events parse_bytewise(const std::string& input) {
    SseParser parser;
    Events events;
    for (const char c : input) {
        parser.feed(std::string_view(&c, 1), [&](std::string_view e, std::string_view d) { events.emplace_back(e, d); });
    }
    return events;
}

}  // namespace

TEST_CASE("SSE: a single event is dispatched on the blank line", "[sse]") {
    const Events events = parse_whole("data: hello\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].first == "message");
    CHECK(events[0].second == "hello");
}

TEST_CASE("SSE: an unfinished event is not dispatched", "[sse]") {
    CHECK(parse_whole("data: hello\n").empty());
    CHECK(parse_whole("data: hello").empty());
}

TEST_CASE("SSE: multiple data lines join with a newline", "[sse]") {
    const Events events = parse_whole("data: one\ndata: two\n\n");
    REQUIRE(events.size() == 1);
    CHECK(events[0].second == "one\ntwo");
}

TEST_CASE("SSE: event names, comments and unknown fields", "[sse]") {
    const Events events = parse_whole(": keep-alive\nevent: ping\nid: 7\nretry: 100\ndata: x\n\ndata: y\n\n");
    REQUIRE(events.size() == 2);
    CHECK(events[0].first == "ping");
    CHECK(events[0].second == "x");
    CHECK(events[1].first == "message");   // the event name does not carry over
    CHECK(events[1].second == "y");
}

TEST_CASE("SSE: only one leading space after the colon is removed", "[sse]") {
    CHECK(parse_whole("data:nospace\n\n")[0].second == "nospace");
    CHECK(parse_whole("data:  two\n\n")[0].second == " two");
    CHECK(parse_whole("data\n\n")[0].second.empty());   // a field with no colon has an empty value
}

TEST_CASE("SSE: CRLF, CR and LF line endings all work", "[sse]") {
    for (const std::string input : {"data: a\r\n\r\ndata: b\r\n\r\n", "data: a\r\rdata: b\r\r", "data: a\n\ndata: b\n\n"}) {
        const Events events = parse_whole(input);
        REQUIRE(events.size() == 2);
        CHECK(events[0].second == "a");
        CHECK(events[1].second == "b");
    }
}

TEST_CASE("SSE: input split at every byte gives the same events", "[sse]") {
    const std::string input =
        "\xEF\xBB\xBF: comment\r\nevent: delta\r\ndata: {\"a\":1}\r\n\r\ndata: line1\ndata: line2\n\ndata: [DONE]\n\n";
    const Events whole = parse_whole(input);
    const Events bytewise = parse_bytewise(input);
    REQUIRE(whole.size() == 3);
    CHECK(whole == bytewise);
    CHECK(whole[0].first == "delta");
    CHECK(whole[0].second == "{\"a\":1}");
    CHECK(whole[1].second == "line1\nline2");
    CHECK(whole[2].second == "[DONE]");
}

TEST_CASE("SSE: a CRLF pair split across two chunks is one line break", "[sse]") {
    SseParser parser;
    Events events;
    const auto handler = [&](std::string_view e, std::string_view d) { events.emplace_back(e, d); };
    parser.feed("data: a\r", handler);
    parser.feed("\n\r", handler);
    parser.feed("\ndata: b\n\n", handler);
    REQUIRE(events.size() == 2);
    CHECK(events[0].second == "a");
    CHECK(events[1].second == "b");
}

TEST_CASE("SSE: an event without data is not dispatched", "[sse]") {
    CHECK(parse_whole("event: ping\n\n").empty());
    CHECK(parse_whole("\n\n\n").empty());
}

TEST_CASE("SSE: an oversized line sets the overflow flag instead of growing without bound", "[sse]") {
    SseParser parser;
    const std::string big(SseParser::kMaxEventBytes + 10, 'x');
    parser.feed("data: ", [](std::string_view, std::string_view) {});
    parser.feed(big, [](std::string_view, std::string_view) {});
    CHECK(parser.overflowed());
}
