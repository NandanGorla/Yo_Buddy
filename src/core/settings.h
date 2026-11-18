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
    std::string hotkey = "Alt+Shift+Space";   // global shortcut: push-to-talk, and shows the panel
    int panel_width = 440;                    // remembered panel size, in DIPs (96 per inch)
    int panel_height = 560;
    bool hide_on_click_away = true;           // hide the panel when another window is clicked
    bool panel_pinned = false;                // pinned: stays open until hidden explicitly
    bool muted = false;                       // tray "Mute"; silences spoken output

    // Voice input: a speech-to-text server the user runs on this PC (loopback only). It is
    // contacted only when the user records something with the voice shortcut.
    std::string voice_base_url = "http://127.0.0.1:8000/v1";
    std::string voice_model = "whisper-1";

    // Spoken output. Windows' built-in voices are the default and need no server. A local neural
    // speech server is optional: an empty speech_base_url means it is not used, so by default no
    // reply text is posted anywhere to be spoken.
    std::string speech_voice;                 // installed Windows voice; empty: the system default
    std::string speech_base_url;              // loopback only
    std::string speech_model;
    int speech_rate = 0;                      // -10..10
    int speech_pitch = 0;                     // -10..10

    // Capture is always user-triggered. These executable/window-name fragments are excluded
    // before a frame can be shown or sent, and preview remains required for remote endpoints.
    std::vector<std::string> capture_exclusions;
    bool capture_send_preview = true;
    std::string overlay_ring_color = "#ffb000";
    std::string overlay_dot_color = "#ffffff";
    std::string overlay_caption_color = "#ffffff";

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
