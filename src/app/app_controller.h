#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/credentials.h"
#include "core/hotkey.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/theme.h"
#include "providers/http.h"
#include "providers/local_endpoint.h"
#include "providers/provider.h"

namespace buddy {

// The brain behind the UI bridge: it receives validated messages from the web UI, owns the
// settings, themes and the conversation, and runs chat requests on a worker thread.
//
// Threading: every public method runs on the UI thread. The worker never touches controller
// state directly; it hands work back through `run_on_ui`.
//
// There is no window code here, so tests can drive the controller with a fake transport.
// Which of Buddy's windows a UI message came from.
enum class ViewId { Main, Panel };

class AppController {
public:
    struct Deps {
        IHttpTransport* transport = nullptr;
        ICredentialStore* credentials = nullptr;
        std::filesystem::path settings_path;
        std::vector<std::filesystem::path> theme_folders;
        std::string app_version;
        // Delivers one protocol message (JSON text) to the web UI. Called on the UI thread only.
        std::function<void(const std::string& json)> send_to_ui;
        // Queues a task to run on the UI thread. Called from the worker thread.
        std::function<void(std::function<void()>)> run_on_ui;

        // Companion shell (M1.5). All optional: without them the related messages do nothing.
        hotkey::IRegistrar* hotkeys = nullptr;                // registers the global hotkey
        std::function<void(ViewId)> hide_window;              // hide the window a message came from
        std::function<void(bool show_settings)> open_main;    // bring up the main window
        std::function<void(const protocol::OverlayDebug&)> overlay_debug;
    };

    explicit AppController(Deps deps);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // Entry point for everything the web UI posts. Replies go to every open window, so the
    // main window and the floating panel always show the same state.
    void handle_ui_message(std::string_view json_text, ViewId source = ViewId::Main);

    // --- companion shell ---
    // Registers the hotkey stored in the settings. Call once the host window exists.
    hotkey::Status apply_stored_hotkey();
    void release_hotkey();
    int hotkey_id() const;                  // the id WM_HOTKEY carries, 0 when no hotkey is live
    hotkey::Status hotkey_status() const;
    std::string hotkey_message() const;     // user-facing text when the hotkey is not active
    std::string hotkey_active() const;      // the live binding, "" when none

    const settings::Settings& settings() const { return settings_; }
    bool first_run() const { return first_run_; }   // no settings file existed at start-up
    void set_panel_size(int width_dip, int height_dip);
    void set_panel_pinned(bool pinned);
    void set_muted(bool muted);

    // Stops a running chat and waits for its worker. Call before the UI goes away.
    void shutdown();

    const theme::Theme& active_theme() const;
    bool chat_in_progress() const { return busy_; }

    // The Credential Manager entry name for a provider's API key.
    static std::string key_name(std::string_view provider_id);

private:
    struct DeltaBuffer;

    void send(const std::string& json) const;
    void send_state() const;

    void on_message(const protocol::AppReady&);
    void on_message(const protocol::ChatSend&);
    void on_message(const protocol::ChatCancel&);
    void on_message(const protocol::ChatClear&);
    void on_message(const protocol::SetTheme&);
    void on_message(const protocol::SetOverlayPalette&);
    void on_message(const protocol::SetProvider&);
    void on_message(const protocol::SetApiKey&);
    void on_message(const protocol::ClearApiKey&);
    void on_message(const protocol::UiLog&);
    void on_message(const protocol::SetHotkey&);
    void on_message(const protocol::SetPanel&);
    void on_message(const protocol::SetCapturePolicy&);
    void on_message(const protocol::OverlayDebug&);
    void on_message(const protocol::WindowHide&);
    void on_message(const protocol::OpenMain&);
    void on_message(const protocol::LocalTestModel&);
    void on_message(const protocol::CaptureRequest&);

    void send_history() const;

    void flush_deltas(const std::string& request_id, DeltaBuffer& buffer);
    void finish_chat(const std::string& request_id, const ChatResult& result);
    void save_settings() const;

    Deps deps_;
    settings::Settings settings_;
    theme::ThemeSet themes_;

    std::vector<ChatMessage> conversation_;   // user and assistant turns, oldest first
    std::string active_request_;
    std::string reply_so_far_;
    std::string active_model_;
    std::optional<ImageInput> pending_capture_;
    bool busy_ = false;
    bool first_run_ = false;
    ViewId current_source_ = ViewId::Main;   // the window whose message is being handled
    std::unique_ptr<hotkey::Manager> hotkey_;

    // Tasks queued for the UI thread capture this flag and do nothing once it is false, so a
    // late task can never touch a destroyed controller.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::jthread worker_;
};

}  // namespace buddy
