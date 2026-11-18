#include "core/caption.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "core/text.h"

namespace buddy::caption {
namespace {

// Only the start of a reply can end up spoken or captioned, so only that much is converted.
constexpr std::size_t kMaxInputBytes = 16 * 1024;
// A reply longer than this is never a caption, whatever it strips down to.
constexpr std::size_t kMaxOverlayInputBytes = 4 * 1024;
// How far ahead a closing emphasis marker is looked for. Keeps the work linear in practice.
constexpr std::size_t kMaxEmphasisSpan = 1'000;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_word(char c) { return text::is_ascii_alnum(static_cast<unsigned char>(c)); }

bool has_fenced_code(std::string_view text) { return text.find("```") != std::string_view::npos; }

bool has_table(std::string_view text) {
    return text.find('|') != std::string_view::npos && text.find("---") != std::string_view::npos;
}

bool has_diagram(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const unsigned char c : text) lower.push_back(static_cast<char>(std::tolower(c)));
    static constexpr std::array<std::string_view, 12> markers{
        "flowchart ", "sequenceDiagram", "classDiagram", "stateDiagram", "erDiagram", "mindmap",
        "gitgraph", "gantt", "journey", "-->", "==>", "<--"};
    for (const std::string_view marker : markers) {
        std::string lower_marker;
        lower_marker.reserve(marker.size());
        for (const unsigned char c : marker) lower_marker.push_back(static_cast<char>(std::tolower(c)));
        if (lower.find(lower_marker) != std::string::npos) return true;
    }
    return lower.find("graph td") != std::string::npos || lower.find("graph lr") != std::string::npos ||
           lower.find("graph tb") != std::string::npos || lower.find("graph bt") != std::string::npos ||
           lower.find("graph rl") != std::string::npos || lower.find("<svg") != std::string::npos;
}

// The number of times `c` repeats from `at`.
std::size_t run_length(std::string_view line, std::size_t at, char c) {
    std::size_t n = 0;
    while (at + n < line.size() && line[at + n] == c) ++n;
    return n;
}

// "[text](target)" starting at `open` (the '['). On success gives the text and the index just
// past the ')'.
bool parse_link(std::string_view line, std::size_t open, std::string_view& text_out, std::size_t& end_out) {
    const std::size_t close = line.find(']', open + 1);
    if (close == std::string_view::npos || close + 1 >= line.size() || line[close + 1] != '(') return false;
    const std::size_t end = line.find(')', close + 2);
    if (end == std::string_view::npos) return false;
    text_out = line.substr(open + 1, close - open - 1);
    end_out = end + 1;
    return true;
}

// Finds the run of `count` markers `c` that closes the emphasis opened just before `from`.
// Code spans in between are skipped: a '*' inside `code` never closes anything.
std::size_t find_emphasis_close(std::string_view line, std::size_t from, char c, std::size_t count) {
    const std::size_t limit = std::min(line.size(), from + kMaxEmphasisSpan);
    for (std::size_t i = from; i < limit;) {
        if (line[i] == '`') {
            const std::size_t ticks = run_length(line, i, '`');
            const std::size_t close = line.find(std::string(ticks, '`'), i + ticks);
            if (close == std::string_view::npos) return std::string_view::npos;
            i = close + ticks;
            continue;
        }
        if (line[i] != c) {
            ++i;
            continue;
        }
        const std::size_t run = run_length(line, i, c);
        const char before = line[i - 1];                               // i > from - 1 >= 0: an opener precedes
        const char after = i + run < line.size() ? line[i + run] : ' ';
        // A closer hugs the text on its left. '_' inside a word (snake_case) is never one.
        if (run == count && i > from && !is_space(before) && (c == '*' || !is_word(after))) return i;
        i += run;
    }
    return std::string_view::npos;
}

// One line with its inline markup removed. What is inside a code span is kept exactly.
std::string strip_inline(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size();) {
        const char c = line[i];

        if (c == '`') {
            const std::size_t ticks = run_length(line, i, '`');
            const std::size_t close = line.find(std::string(ticks, '`'), i + ticks);
            if (close == std::string_view::npos) {
                i += ticks;   // a stray backtick: drop it, keep reading
                continue;
            }
            out.append(text::trim_ascii(line.substr(i + ticks, close - i - ticks)));
            i = close + ticks;
            continue;
        }

        if (c == '[' || (c == '!' && i + 1 < line.size() && line[i + 1] == '[')) {
            std::string_view label;
            std::size_t end = 0;
            if (parse_link(line, c == '!' ? i + 1 : i, label, end)) {
                out += strip_inline(label);   // a link reads as its text, an image as its description
                i = end;
                continue;
            }
        }

        if (c == '*' || c == '_') {
            const std::size_t run = run_length(line, i, c);
            const char before = i > 0 ? line[i - 1] : ' ';
            const char after = i + run < line.size() ? line[i + run] : ' ';
            // An opener hugs the text on its right. "2 * 3", "int* p" and "snake_case" have none.
            const bool can_open = run <= 3 && !is_space(after) && (c == '*' || !is_word(before));
            const std::size_t close =
                can_open ? find_emphasis_close(line, i + run, c, run) : std::string_view::npos;
            if (close != std::string_view::npos) {
                out += strip_inline(line.substr(i + run, close - i - run));
                i = close + run;
            } else {
                out.append(run, c);   // not emphasis: an operator, a pointer, part of a name
                i += run;
            }
            continue;
        }

        out.push_back(c);
        ++i;
    }
    return out;
}

// Removes what marks a line as a heading, a quote or a list item.
std::string_view strip_block_marker(std::string_view line) {
    while (!line.empty() && line.front() == '>') line = text::trim_ascii(line.substr(1));

    // A heading is one to six '#' and then a space. "#include" is not a heading.
    const std::size_t hashes = run_length(line, 0, '#');
    if (hashes >= 1 && hashes <= 6 && (hashes == line.size() || line[hashes] == ' ')) {
        return text::trim_ascii(line.substr(hashes));
    }
    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
        return text::trim_ascii(line.substr(2));
    }
    return line;
}

void append_collapsed(std::string& out, std::string_view piece) {
    bool pending_space = !out.empty();
    for (const char c : piece) {
        if (is_space(c)) {
            pending_space = true;
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    }
}

}  // namespace

std::string to_plain(std::string_view markdown) {
    const std::string head = text::truncate_utf8(markdown, kMaxInputBytes);
    const std::string_view input = head;

    std::string out;
    out.reserve(input.size());
    std::string_view fence;   // the marker that opened the code block we are inside, if any
    for (std::size_t i = 0; i < input.size();) {
        const std::size_t nl = input.find('\n', i);
        const std::string_view raw = input.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
        i = nl == std::string_view::npos ? input.size() : nl + 1;
        const std::string_view line = text::trim_ascii(raw);

        // Fenced code is skipped whole. The fence may be indented (inside a list item), and a
        // fence that is never closed swallows the rest: better silence than reading code aloud.
        if (!fence.empty()) {
            if (line.substr(0, 3) == fence) fence = {};
            continue;
        }
        // (A line that opens and closes on its own, "```x```", is inline code, handled below.)
        const std::string_view marker = line.substr(0, 3);
        if ((marker == "```" || marker == "~~~") && line.find(marker, 3) == std::string_view::npos) {
            fence = marker;
            continue;
        }

        const std::string piece = strip_inline(strip_block_marker(line));
        const std::string_view visible = text::trim_ascii(piece);
        if (!visible.empty()) append_collapsed(out, visible);
    }
    return out;
}

bool overlay_ok(std::string_view markdown) {
    if (markdown.size() > kMaxOverlayInputBytes || has_fenced_code(markdown) || has_table(markdown) ||
        has_diagram(markdown)) return false;
    const std::string plain = to_plain(markdown);
    return !plain.empty() && plain.size() <= kMaxOverlayBytes;
}

std::string for_overlay(std::string_view markdown) {
    if (!overlay_ok(markdown)) return {};
    return to_plain(markdown);
}

std::string for_speech(std::string_view markdown) {
    std::string plain = to_plain(markdown);
    if (plain.empty() && !text::trim_ascii(markdown).empty()) {
        plain = "The reply is in the chat panel.";
    }
    return text::truncate_utf8(plain, kMaxSpeechBytes);
}

}  // namespace buddy::caption
