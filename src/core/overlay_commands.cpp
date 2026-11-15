#include "core/overlay_commands.h"

#include <cmath>
#include <initializer_list>

#include <nlohmann/json.hpp>

namespace buddy::overlay {
namespace {

using json = nlohmann::json;
constexpr double kMaxCoordinate = 100000.0;

bool exact_keys(const json& object, std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) return false;
    for (const char* key : keys) {
        if (!object.contains(key)) return false;
    }
    return true;
}

bool number(const json& object, const char* key, double& value) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) return false;
    value = it->get<double>();
    return std::isfinite(value);
}

bool coordinate(double value) { return value >= 0.0 && value <= kMaxCoordinate; }

bool extent(double value) { return value > 0.0 && value <= kMaxCoordinate; }

bool safe_label(const std::string& value) {
    if (value.empty() || value.size() > kMaxLabelBytes) return false;
    for (unsigned char c : value) {
        if (c < 0x20 && c != '\n' && c != '\t') return false;
    }
    return true;
}

ParseResult fail(std::string message) { return {{}, std::move(message)}; }

}  // namespace

ParseResult parse_commands(std::string_view json_text) {
    const json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded()) return fail("commands are not valid JSON");
    if (!exact_keys(root, {"commands"})) return fail("expected an object containing only commands");
    if (!root["commands"].is_array()) return fail("commands must be an array");
    if (root["commands"].size() > kMaxCommands) return fail("too many drawing commands");

    std::vector<Command> commands;
    commands.reserve(root["commands"].size());
    for (const json& item : root["commands"]) {
        if (!item.is_object() || !item.contains("type") || !item["type"].is_string()) {
            return fail("each command needs a string type");
        }
        const std::string type = item["type"].get<std::string>();
        if (type == "arrow") {
            Arrow command{};
            if (!exact_keys(item, {"type", "x1", "y1", "x2", "y2"}) ||
                !number(item, "x1", command.x1) || !number(item, "y1", command.y1) ||
                !number(item, "x2", command.x2) || !number(item, "y2", command.y2) ||
                !coordinate(command.x1) || !coordinate(command.y1) ||
                !coordinate(command.x2) || !coordinate(command.y2)) {
                return fail("invalid arrow command");
            }
            commands.emplace_back(command);
        } else if (type == "circle") {
            Circle command{};
            if (!exact_keys(item, {"type", "x", "y", "radius"}) ||
                !number(item, "x", command.x) || !number(item, "y", command.y) ||
                !number(item, "radius", command.radius) || !coordinate(command.x) ||
                !coordinate(command.y) || !extent(command.radius)) {
                return fail("invalid circle command");
            }
            commands.emplace_back(command);
        } else if (type == "highlight") {
            Highlight command{};
            if (!exact_keys(item, {"type", "x", "y", "width", "height"}) ||
                !number(item, "x", command.x) || !number(item, "y", command.y) ||
                !number(item, "width", command.width) || !number(item, "height", command.height) ||
                !coordinate(command.x) || !coordinate(command.y) ||
                !extent(command.width) || !extent(command.height)) {
                return fail("invalid highlight command");
            }
            commands.emplace_back(command);
        } else if (type == "label") {
            Label command{};
            const auto text = item.find("text");
            if (!exact_keys(item, {"type", "x", "y", "text"}) ||
                !number(item, "x", command.x) || !number(item, "y", command.y) ||
                !coordinate(command.x) || !coordinate(command.y) ||
                text == item.end() || !text->is_string()) {
                return fail("invalid label command");
            }
            command.text = text->get<std::string>();
            if (!safe_label(command.text)) return fail("invalid label text");
            commands.emplace_back(std::move(command));
        } else if (type == "pulse") {
            Pulse command{};
            const auto duration = item.find("duration_ms");
            if (!exact_keys(item, {"type", "x", "y", "radius", "duration_ms"}) ||
                !number(item, "x", command.x) || !number(item, "y", command.y) ||
                !number(item, "radius", command.radius) || !coordinate(command.x) ||
                !coordinate(command.y) || !extent(command.radius) ||
                duration == item.end() || !duration->is_number_integer()) {
                return fail("invalid pulse command");
            }
            command.duration_ms = duration->get<int>();
            if (command.duration_ms < 100 || command.duration_ms > 10000) {
                return fail("pulse duration must be between 100 and 10000 milliseconds");
            }
            commands.emplace_back(command);
        } else {
            return fail("unknown drawing command type");
        }
    }
    return {std::move(commands), {}};
}

}  // namespace buddy::overlay
