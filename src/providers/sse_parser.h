#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace buddy {

// Incremental parser for Server-Sent Events (the "text/event-stream" format that streaming chat
// APIs use). Feed it the response body in whatever pieces the network delivers; it calls the
// handler once per complete event. Pieces may split an event, a line or even a CRLF pair.
//
// Rules implemented (from the WHATWG HTML "server-sent events" section):
//  - lines end with LF, CR or CRLF
//  - a line starting with ':' is a comment
//  - "data:" lines accumulate, joined by '\n'; a blank line dispatches the event
//  - one leading space after the colon is dropped
//  - an event with no data is not dispatched; an unfinished event at end of stream is discarded
class SseParser {
public:
    using Handler = std::function<void(std::string_view event, std::string_view data)>;

    void feed(std::string_view chunk, const Handler& handler);

    // True once a line or event grew past the safety limit. The stream should then be abandoned.
    bool overflowed() const { return overflowed_; }

    static constexpr std::size_t kMaxEventBytes = 4 * 1024 * 1024;

private:
    void process_line(const Handler& handler);

    std::string line_;
    std::string event_;
    std::string data_;
    bool has_data_ = false;
    bool skip_lf_ = false;       // the previous byte was CR: swallow one LF if it comes next
    bool first_line_ = true;     // a UTF-8 byte order mark may precede the first line
    bool overflowed_ = false;
};

}  // namespace buddy
