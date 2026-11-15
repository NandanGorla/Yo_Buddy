#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Non-secret settings, stored as JSON in %LOCALAPPDATA%\Buddy\settings.json.
// API keys are never written here: they live in Windows Credential Manager.
namespace buddy::settings {

struct ProviderConfig {
    std::string id;                 // "anthropic", "openai", or "local"
    std::string label;              // shown in the UI
    std::string base_url;           // OpenAI-compatible endpoint, without "/chat/completions"
    std::string model;              // may be empty until the user picks one
    std::string default_base_url;
    std::string default_model;
    int max_output_tokens = 0;      // 0 = let the server decide
};

struct Settings {
    std::string theme = "dark";
    std::string provider = "anthropic";
    std::vector<ProviderConfig> providers;

    // Companion shell (M1.5).
    std::string hotkey = "Alt+Shift+Space";   // global shortcut that toggles the floating panel
    int panel_width = 440;                    // remembered panel size, in DIPs (96 per inch)
    int panel_height = 560;
    bool hide_on_click_away = true;           // hide the panel when another window is clicked
    bool panel_pinned = false;                // pinned: stays open until hidden explicitly
    bool muted = false;                       // tray "Mute"; silences spoken output once M3 adds it

    const ProviderConfig* find(std::string_view id) const;
    ProviderConfig* find(std::string_view id);
    const ProviderConfig& active() const;   // falls back to the first provider
};

// The built-in provider list with default endpoints and models.
Settings defaults();

// Parses stored settings. Anything missing or invalid falls back to its default and is reported
// in `warnings` (when given), so a damaged file can never stop the app from starting.
Settings from_json(std::string_view text, std::vector<std::string>* warnings = nullptr);
std::string to_json(const Settings& settings);

Settings load(const std::filesystem::path& file, std::vector<std::string>* warnings = nullptr);
bool save(const std::filesystem::path& file, const Settings& settings);

}  // namespace buddy::settings
