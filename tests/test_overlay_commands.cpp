#include <catch2/catch_test_macros.hpp>

#include "core/overlay_commands.h"

using namespace buddy::overlay;

TEST_CASE("overlay commands: valid drawing schema parses", "[overlay]") {
    const auto result = parse_commands(
        R"({"commands":[{"type":"arrow","x1":10,"y1":20,"x2":100,"y2":120},)"
        R"({"type":"circle","x":100,"y":120,"radius":24},)"
        R"({"type":"highlight","x":5,"y":6,"width":200,"height":80},)"
        R"({"type":"label","x":12,"y":14,"text":"Click here"},)"
        R"({"type":"pulse","x":100,"y":120,"radius":32,"duration_ms":800}]})");
    REQUIRE(result.ok());
    REQUIRE(result.commands.size() == 5);
    CHECK(std::get<Arrow>(result.commands[0]).x2 == 100);
    CHECK(std::get<Label>(result.commands[3]).text == "Click here");
    CHECK(std::get<Pulse>(result.commands[4]).duration_ms == 800);
}

TEST_CASE("overlay commands: malformed or unsafe commands are rejected", "[overlay]") {
    CHECK_FALSE(parse_commands("").ok());
    CHECK_FALSE(parse_commands(R"({"commands":[{"type":"triangle"}]})").ok());
    CHECK_FALSE(parse_commands(R"({"commands":[{"type":"circle","x":0,"y":0,"radius":0}]})").ok());
    CHECK_FALSE(parse_commands(R"({"commands":[{"type":"label","x":0,"y":0,"text":"\u0001"}]})").ok());
    CHECK_FALSE(parse_commands(R"({"commands":[{"type":"pulse","x":0,"y":0,"radius":5,"duration_ms":99}]})").ok());
    CHECK_FALSE(parse_commands(R"({"commands":[{"type":"arrow","x1":0,"y1":0,"x2":1,"y2":1,"extra":1}]})").ok());
}

TEST_CASE("overlay commands: command count is bounded", "[overlay]") {
    std::string input = R"({"commands":[)";
    for (std::size_t i = 0; i < kMaxCommands + 1; ++i) {
        if (i != 0) input += ',';
        input += R"({"type":"circle","x":1,"y":1,"radius":1})";
    }
    input += "]})";
    CHECK_FALSE(parse_commands(input).ok());
}
