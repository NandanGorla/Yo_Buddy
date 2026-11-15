#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace buddy::overlay {

struct Arrow {
    double x1;
    double y1;
    double x2;
    double y2;
};

struct Circle {
    double x;
    double y;
    double radius;
};

struct Highlight {
    double x;
    double y;
    double width;
    double height;
};

struct Label {
    double x;
    double y;
    std::string text;
};

struct Pulse {
    double x;
    double y;
    double radius;
    int duration_ms;
};

using Command = std::variant<Arrow, Circle, Highlight, Label, Pulse>;

struct ParseResult {
    std::vector<Command> commands;
    std::string error;

    bool ok() const { return error.empty(); }
};

inline constexpr std::size_t kMaxCommands = 64;
inline constexpr std::size_t kMaxLabelBytes = 512;

ParseResult parse_commands(std::string_view json_text);

}  // namespace buddy::overlay
