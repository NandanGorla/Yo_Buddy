#include "core/theme.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/protocol.h"
#include "core/text.h"

namespace buddy::theme {
namespace {

using json = nlohmann::json;

enum class TokenType { Color, Length, FontList };

struct TokenSpec {
    std::string_view name;
    TokenType type;
};

// The complete token vocabulary. A theme must define every one of these and nothing else.
constexpr std::array<TokenSpec, 14> kUiTokens = {{
    {"bg", TokenType::Color},
    {"surface", TokenType::Color},
    {"surface-raised", TokenType::Color},
    {"text", TokenType::Color},
    {"text-muted", TokenType::Color},
    {"accent", TokenType::Color},
    {"accent-text", TokenType::Color},
    {"border", TokenType::Color},
    {"danger", TokenType::Color},
    {"code-bg", TokenType::Color},
    {"code-text", TokenType::Color},
    {"radius", TokenType::Length},
    {"font-ui", TokenType::FontList},
    {"font-mono", TokenType::FontList},
}};

constexpr std::array<TokenSpec, 3> kOverlayTokens = {{
    {"highlight", TokenType::Color},
    {"label-bg", TokenType::Color},
    {"label-text", TokenType::Color},
}};

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_valid_value(TokenType type, std::string_view value) {
    switch (type) {
        case TokenType::Color: return is_valid_color(value);
        case TokenType::Length: return is_valid_length(value);
        case TokenType::FontList: return is_valid_font_list(value);
    }
    return false;
}

const char* type_name(TokenType type) {
    switch (type) {
        case TokenType::Color: return "a hex colour such as #1a2b3c";
        case TokenType::Length: return "a length such as 8px";
        case TokenType::FontList: return "a comma-separated list of font names";
    }
    return "valid";
}

ParseResult fail(std::string message) {
    ParseResult r;
    r.error = std::move(message);
    return r;
}

// Reads one token group ("tokens" or "overlay") against its spec table.
template <std::size_t N>
std::string read_group(const json& root, const char* group, const std::array<TokenSpec, N>& specs,
                       std::map<std::string, std::string>& out) {
    const auto it = root.find(group);
    if (it == root.end() || !it->is_object()) return std::string("\"") + group + "\" must be an object";
    const json& obj = *it;

    for (const auto& item : obj.items()) {
        const bool known = std::any_of(specs.begin(), specs.end(),
                                       [&](const TokenSpec& s) { return s.name == item.key(); });
        if (!known) return std::string("unknown token in \"") + group + "\"";
    }
    for (const TokenSpec& spec : specs) {
        const auto tok = obj.find(std::string(spec.name));
        if (tok == obj.end()) return "missing token \"" + std::string(spec.name) + "\"";
        if (!tok->is_string()) return "token \"" + std::string(spec.name) + "\" must be a string";
        const std::string& value = tok->get_ref<const std::string&>();
        if (!is_valid_value(spec.type, value)) {
            return "token \"" + std::string(spec.name) + "\" must be " + type_name(spec.type);
        }
        out.emplace(std::string(spec.name), value);
    }
    return {};
}

bool is_valid_display_name(std::string_view name) {
    if (name.empty() || name.size() > 48) return false;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!text::is_ascii_alnum(c) && c != ' ' && c != '-' && c != '_' && c != '(' && c != ')') return false;
    }
    return true;
}

}  // namespace

bool is_valid_color(std::string_view value) {
    if (value.size() != 4 && value.size() != 7 && value.size() != 9) return false;
    if (value.front() != '#') return false;
    return std::all_of(value.begin() + 1, value.end(), is_hex_digit);
}

bool is_valid_length(std::string_view value) {
    if (value == "0") return true;
    std::size_t i = 0;
    std::size_t digits = 0;
    while (i < value.size() && is_digit(value[i])) { ++i; ++digits; }
    if (digits == 0 || digits > 3) return false;
    if (i < value.size() && value[i] == '.') {
        ++i;
        std::size_t decimals = 0;
        while (i < value.size() && is_digit(value[i])) { ++i; ++decimals; }
        if (decimals == 0 || decimals > 2) return false;
    }
    const std::string_view unit = value.substr(i);
    return unit == "px" || unit == "rem" || unit == "em";
}

bool is_valid_font_list(std::string_view value) {
    if (value.empty() || value.size() > 200) return false;
    std::string_view rest = value;
    while (true) {
        const auto comma = rest.find(',');
        const std::string_view name = text::trim_ascii(rest.substr(0, comma));
        if (name.empty() || name.size() > 64) return false;
        // Font names are written unquoted, so each word must be a CSS identifier:
        // letters, digits and hyphens, not starting with a digit.
        bool word_start = true;
        for (const char ch : name) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == ' ') { word_start = true; continue; }
            if (!text::is_ascii_alnum(c) && c != '-') return false;
            if (word_start && (is_digit(ch) || c == '-')) return false;
            word_start = false;
        }
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return true;
}

ParseResult parse_theme(std::string_view json_text) {
    if (json_text.size() > kMaxThemeFileBytes) return fail("theme file is too large");

    const json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded()) return fail("not valid JSON");
    if (!root.is_object()) return fail("theme must be a JSON object");

    for (const auto& item : root.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "id" && key != "name" && key != "kind" && key != "tokens" &&
            key != "overlay") {
            return fail("unknown top-level key");
        }
    }

    const auto schema = root.find("schema");
    if (schema == root.end() || !schema->is_number_integer() || schema->get<std::int64_t>() != 1) {
        return fail("\"schema\" must be 1");
    }

    Theme theme;
    const auto id = root.find("id");
    if (id == root.end() || !id->is_string() || !protocol::is_valid_id(id->get_ref<const std::string&>())) {
        return fail("\"id\" must be lower-case letters, digits, '-' or '_'");
    }
    theme.id = id->get<std::string>();

    const auto name = root.find("name");
    if (name == root.end() || !name->is_string() ||
        !is_valid_display_name(name->get_ref<const std::string&>())) {
        return fail("\"name\" must be 1 to 48 plain characters");
    }
    theme.name = name->get<std::string>();

    const auto kind = root.find("kind");
    if (kind == root.end() || !kind->is_string()) return fail("\"kind\" is missing");
    theme.kind = kind->get<std::string>();
    if (theme.kind != "light" && theme.kind != "dark" && theme.kind != "high-contrast") {
        return fail("\"kind\" must be light, dark or high-contrast");
    }

    if (std::string err = read_group(root, "tokens", kUiTokens, theme.tokens); !err.empty()) return fail(err);
    if (std::string err = read_group(root, "overlay", kOverlayTokens, theme.overlay); !err.empty()) return fail(err);

    return ParseResult{std::move(theme), {}};
}

const Theme& builtin_theme() {
    static const Theme theme = [] {
        Theme t;
        t.id = "builtin-light";
        t.name = "Light (built in)";
        t.kind = "light";
        t.tokens = {
            {"bg", "#f6f7f9"},          {"surface", "#ffffff"},   {"surface-raised", "#eef0f4"},
            {"text", "#1c1f24"},        {"text-muted", "#5b6370"}, {"accent", "#2456d6"},
            {"accent-text", "#ffffff"}, {"border", "#d5d9e0"},    {"danger", "#b3261e"},
            {"code-bg", "#eef0f4"},     {"code-text", "#1c1f24"}, {"radius", "8px"},
            {"font-ui", "Segoe UI Variable Text, Segoe UI, sans-serif"},
            {"font-mono", "Cascadia Code, Consolas, monospace"},
        };
        t.overlay = {{"highlight", "#ffb000"}, {"label-bg", "#1c1f24e6"}, {"label-text", "#ffffff"}};
        return t;
    }();
    return theme;
}

ThemeSet load_themes(const std::vector<std::filesystem::path>& folders) {
    ThemeSet set;
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) continue;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".json") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());   // stable order regardless of the file system

        for (const auto& file : files) {
            const std::string file_name = file.filename().string();
            const auto size = std::filesystem::file_size(file, ec);
            if (ec || size > kMaxThemeFileBytes) {
                set.issues.push_back({file_name, "file is too large or unreadable"});
                continue;
            }
            std::ifstream in(file, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            if (!in) {
                set.issues.push_back({file_name, "could not read the file"});
                continue;
            }
            ParseResult parsed = parse_theme(buffer.str());
            if (!parsed.ok()) {
                set.issues.push_back({file_name, parsed.error});
                continue;
            }
            const auto existing = std::find_if(set.themes.begin(), set.themes.end(),
                                               [&](const Theme& t) { return t.id == parsed.theme->id; });
            if (existing != set.themes.end()) {
                *existing = std::move(*parsed.theme);
            } else {
                set.themes.push_back(std::move(*parsed.theme));
            }
        }
    }
    if (set.themes.empty()) set.themes.push_back(builtin_theme());
    return set;
}

const Theme& pick_theme(const ThemeSet& set, std::string_view id) {
    for (const Theme& t : set.themes) {
        if (t.id == id) return t;
    }
    if (!set.themes.empty()) return set.themes.front();
    return builtin_theme();
}

nlohmann::json to_json(const Theme& theme) {
    return json{{"id", theme.id},
                {"name", theme.name},
                {"kind", theme.kind},
                {"tokens", theme.tokens},
                {"overlay", theme.overlay}};
}

}  // namespace buddy::theme
