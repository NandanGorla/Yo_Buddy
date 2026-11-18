#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

// The bridge between the web UI and the C++ host.
//
// Every message, in both directions, is one JSON object with exactly three keys:
//
//     { "v": 1, "type": "chat.send", "payload": { ... } }
//
// "v" is the protocol version. A side that sees a version it does not speak rejects the message.
// Both sides validate strictly: unknown types, unknown fields, wrong JSON types and oversized
// strings are all rejected. The JavaScript mirror of these rules lives in ui/js/protocol.js;
// change the two files together.
namespace buddy::protocol {

inline constexpr int kVersion = 1;

inline constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
inline constexpr std::size_t kMaxChatTextBytes = 200 * 1000;
inline constexpr std::size_t kMaxIdLength = 64;
inline constexpr std::size_t kMaxModelLength = 128;
inline constexpr std::size_t kMaxBaseUrlLength = 2048;
inline constexpr std::size_t kMaxSpeechVoiceLength = 512;   // name of an installed Windows voice
inline constexpr std::size_t kMaxApiKeyLength = 512;
inline constexpr std::size_t kMaxLogLength = 2000;
inline constexpr std::size_t kMaxHotkeyLength = 64;
inline constexpr std::size_t kMaxCaptureExclusions = 64;
inline constexpr std::size_t kMaxCaptureExclusionLength = 128;

// ---- UI -> host -------------------------------------------------------------------------------

struct AppReady {};                                  // "app.ready"            {}
struct ChatSend {                                    // "chat.send"            {request_id, text}
    std::string request_id;
    std::string text;
};
struct ChatCancel { std::string request_id; };       // "chat.cancel"          {request_id}
struct ChatClear {};                                 // "chat.clear"           {}
struct VoiceCancel {};                               // "voice.cancel"         {}
struct SetTheme { std::string theme; };              // "settings.set_theme"   {theme}
struct SetOverlayPalette {                           // "settings.set_overlay" {ring, dot, caption}
    std::string ring;
    std::string dot;
    std::string caption;
};
struct SetProvider {                                 // "settings.set_provider" {provider, base_url, model}
    std::string provider;
    std::string base_url;
    std::string model;                               // may be empty: "not chosen yet"
};
struct SetVoice {                                    // "settings.set_voice"   {base_url, model}
    std::string base_url;                            // the local speech-to-text server
    std::string model;
};
struct SetSpeech {                                   // "settings.set_speech"  {voice, base_url, model, rate, pitch}
    std::string voice;                               // an installed Windows voice; empty: the system default
    std::string base_url;                            // optional local neural speech server; empty: not used
    std::string model;                               // its model; empty when base_url is
    int rate = 0;                                    // -10..10
    int pitch = 0;                                   // -10..10
};
struct SetApiKey {                                   // "settings.set_api_key" {provider, key}
    std::string provider;
    std::string key;
};
struct ClearApiKey { std::string provider; };        // "settings.clear_api_key" {provider}
struct UiLog {                                       // "ui.log"               {level, message}
    std::string level;                               // "info" | "warn" | "error"
    std::string message;
};

struct SetHotkey { std::string hotkey; };            // "settings.set_hotkey"  {hotkey}
struct SetPanel {                                    // "settings.set_panel"   {hide_on_click_away, pinned}
    bool hide_on_click_away = true;
    bool pinned = false;
};
struct SetCapturePolicy {                            // "settings.set_capture" {exclusions, send_preview}
    std::vector<std::string> exclusions;
    bool send_preview = true;
};
struct OverlayDebug {                               // "overlay.debug" {action, caption?}
    std::string action;
    std::string caption;
};
struct WindowHide {};                                // "window.hide"          {}   hide the window that sent it
struct OpenMain { bool settings = false; };          // "window.open_main"     {settings}
struct LocalTestModel {                              // "local.test_model"    {endpoint, model}
    std::string endpoint;
    std::string model;
};
struct CaptureRequest {                              // "capture.request"     {target, preview}
    std::string target;                              // "monitor" | "window"
    bool preview = true;
};

using UiMessage = std::variant<AppReady, ChatSend, ChatCancel, ChatClear, VoiceCancel, SetTheme, SetOverlayPalette, SetProvider,
                               SetVoice,
                               SetSpeech,
                               SetApiKey, ClearApiKey, UiLog, SetHotkey, SetPanel, WindowHide, OpenMain,
                               LocalTestModel, SetCapturePolicy, OverlayDebug, CaptureRequest>;

struct ParseResult {
    std::optional<UiMessage> message;
    std::string error_code;    // "invalid_json", "too_large", "invalid_envelope",
                               // "unsupported_version", "unknown_type", "invalid_payload"
    std::string error_detail;  // human-readable, safe to log (never contains payload values)

    bool ok() const { return message.has_value(); }
};

ParseResult parse_ui_message(std::string_view json_text);

// Shared field rules, also used by settings validation.
bool is_valid_id(std::string_view s);        // [a-z0-9_-]{1,64}, used for provider, theme and request ids
bool is_valid_model(std::string_view s);     // empty, or [A-Za-z0-9._:/@-]{1,128}
bool is_valid_api_key(std::string_view s);   // printable ASCII, 1..512, no whitespace

// ---- host -> UI -------------------------------------------------------------------------------

// Wraps a payload in the versioned envelope and serialises it. Invalid UTF-8 inside strings is
// replaced, never thrown on, so a hostile model reply cannot break the bridge.
std::string make_message(std::string_view type, nlohmann::json payload);

std::string app_state(nlohmann::json payload);                                     // "app.state"
// "chat.started" carries the user text so that a second window showing the same conversation
// (main window and floating panel) can display the question it did not send itself.
std::string chat_started(std::string_view request_id, std::string_view provider,
                         std::string_view model, std::string_view user_text);       // "chat.started"
// "chat.history": the whole conversation, sent when a window (re)loads. `messages` is an array
// of {role: "user"|"assistant", text}. When a reply is streaming, active_request names it and
// `partial` holds the text received so far.
std::string chat_history(nlohmann::json messages, std::string_view active_request,
                         std::string_view active_model, std::string_view partial);  // "chat.history"
std::string focus_input();                                                         // "ui.focus_input"
std::string show_settings();                                                       // "ui.show_settings"
std::string chat_delta(std::string_view request_id, std::string_view text);        // "chat.delta"
std::string chat_done(std::string_view request_id, std::string_view stop_reason,
                      std::optional<std::int64_t> input_tokens,
                      std::optional<std::int64_t> output_tokens);                  // "chat.done"
std::string chat_error(std::string_view request_id, std::string_view code,
                       std::string_view message);                                  // "chat.error"
std::string chat_cleared();                                                        // "chat.cleared"
std::string host_error(std::string_view code, std::string_view message);           // "host.error"
std::string local_test_result(std::string_view model, bool vision, std::string_view message); // "local.test_result"
std::string capture_preview(std::string_view media_type, std::string_view base64,
                            int width, int height); // "capture.preview"
// state: idle | listening | listening_toggle | transcribing | error
std::string voice_state(std::string_view state, std::string_view error); // "voice.state"

}  // namespace buddy::protocol
