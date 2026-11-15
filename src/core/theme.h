#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Themes are data, never code. A theme file is JSON holding a fixed set of design tokens:
//
//   {
//     "schema": 1,
//     "id": "dark", "name": "Dark", "kind": "dark",
//     "tokens":  { "bg": "#14161a", ..., "radius": "8px", "font-ui": "Segoe UI, sans-serif", ... },
//     "overlay": { "highlight": "#ffcc00", "label-bg": "#000000cc", "label-text": "#ffffff" }
//   }
//
// Each token has a type (colour, length or font list) and its value must match that type exactly.
// That is what keeps a theme file from smuggling in CSS such as url(...) or extra declarations:
// only values that pass here are ever handed to the web UI or (later) to the Direct2D overlay.
namespace buddy::theme {

struct Theme {
    std::string id;
    std::string name;
    std::string kind;                             // "light" | "dark" | "high-contrast"
    std::map<std::string, std::string> tokens;    // web UI tokens, become CSS variables --bd-<name>
    std::map<std::string, std::string> overlay;   // tokens for the on-screen overlay (M4)
};

struct ParseResult {
    std::optional<Theme> theme;
    std::string error;

    bool ok() const { return theme.has_value(); }
};

inline constexpr std::size_t kMaxThemeFileBytes = 64 * 1024;

ParseResult parse_theme(std::string_view json_text);

// Individual value checks, exposed for tests.
bool is_valid_color(std::string_view value);    // #rgb, #rrggbb or #rrggbbaa
bool is_valid_length(std::string_view value);   // 0, or up to 3 digits (+2 decimals) with px, rem or em
bool is_valid_font_list(std::string_view value);

struct LoadIssue {
    std::string file;
    std::string problem;
};

struct ThemeSet {
    std::vector<Theme> themes;       // never empty: the built-in theme is used when nothing loads
    std::vector<LoadIssue> issues;   // files that were skipped, for the log
};

// Loads every *.json theme from the given folders. Later folders win when ids collide, so a user
// folder can override a shipped theme. Invalid files are skipped and reported in `issues`.
ThemeSet load_themes(const std::vector<std::filesystem::path>& folders);

// The compiled-in fallback, so the UI always has a complete token set.
const Theme& builtin_theme();

// Returns the theme with this id, or a sensible default when it does not exist.
const Theme& pick_theme(const ThemeSet& set, std::string_view id);

nlohmann::json to_json(const Theme& theme);

}  // namespace buddy::theme
