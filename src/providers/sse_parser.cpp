#include "providers/sse_parser.h"

namespace buddy {

void SseParser::feed(std::string_view chunk, const Handler& handler) {
    for (const char c : chunk) {
        if (skip_lf_) {
            skip_lf_ = false;
            if (c == '\n') continue;   // second half of a CRLF pair
        }
        if (c == '\r') {
            process_line(handler);
            skip_lf_ = true;
        } else if (c == '\n') {
            process_line(handler);
        } else if (line_.size() < kMaxEventBytes) {
            line_.push_back(c);
        } else {
            overflowed_ = true;
        }
    }
}

void SseParser::process_line(const Handler& handler) {
    std::string line = std::move(line_);
    line_.clear();

    if (first_line_) {
        first_line_ = false;
        if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    }

    if (line.empty()) {
        // Blank line: the event is complete.
        if (has_data_ && handler) {
            handler(event_.empty() ? std::string_view("message") : std::string_view(event_), data_);
        }
        event_.clear();
        data_.clear();
        has_data_ = false;
        return;
    }
    if (line.front() == ':') return;   // comment, often used as a keep-alive

    const auto colon = line.find(':');
    const std::string_view field = std::string_view(line).substr(0, colon);
    std::string_view value;
    if (colon != std::string::npos) {
        value = std::string_view(line).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }

    if (field == "event") {
        event_ = std::string(value);
    } else if (field == "data") {
        if (data_.size() + value.size() + 1 > kMaxEventBytes) {
            overflowed_ = true;
            return;
        }
        if (has_data_) data_.push_back('\n');
        data_.append(value);
        has_data_ = true;
    }
    // "id" and "retry" exist in the format but mean nothing for a one-shot chat stream.
}

}  // namespace buddy
