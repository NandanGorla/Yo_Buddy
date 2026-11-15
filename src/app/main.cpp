// Buddy: the Win32 host process.
//
// Buddy lives in the notification area ("tray"). A hidden host window owns the tray icon, the
// global hotkey and the single-instance handshake. Two windows show the web UI:
//   - the floating panel, toggled by the hotkey (app/panel_window.*)
//   - the main window with chat and settings, opened from the tray (app/main_window.*)
// Both are views of the same AppController, so they always show the same conversation.

#include <cwctype>
#include <memory>
#include <string>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "app/app_controller.h"
#include "app/main_window.h"
#include "app/overlay_window.h"
#include "app/panel_window.h"
#include "app/win_util.h"
#include "core/hotkey.h"
#include "core/log.h"
#include "platform/credential_store.h"
#include "platform/paths.h"
#include "platform/win_text.h"
#include "platform/winhttp_transport.h"

#ifndef BUDDY_VERSION
#define BUDDY_VERSION "0.0.0"
#endif

namespace {

using namespace buddy;

// Tray menu command ids.
constexpr UINT kCmdOpen = 1001;
constexpr UINT kCmdSettings = 1002;
constexpr UINT kCmdMute = 1003;
constexpr UINT kCmdQuit = 1004;
constexpr UINT kTrayIconId = 1;

// The real RegisterHotKey behind hotkey::IRegistrar.
class WinHotkeyRegistrar final : public hotkey::IRegistrar {
public:
    void attach(HWND hwnd) { hwnd_ = hwnd; }

    hotkey::RegisterOutcome register_hotkey(int id, const hotkey::Binding& binding) override {
        // MOD_NOREPEAT: holding the keys down sends one WM_HOTKEY, not a stream of them.
        if (RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.key)) return hotkey::RegisterOutcome::Ok;
        const DWORD error = GetLastError();
        log::warn("RegisterHotKey failed with error " + std::to_string(error));
        return error == ERROR_HOTKEY_ALREADY_REGISTERED ? hotkey::RegisterOutcome::AlreadyTaken
                                                        : hotkey::RegisterOutcome::Failed;
    }
    void unregister_hotkey(int id) override { UnregisterHotKey(hwnd_, id); }

private:
    HWND hwnd_ = nullptr;
};

class App {
public:
    App(HINSTANCE instance, std::wstring profile) : instance_(instance), profile_(std::move(profile)) {}

    // The window class of the hidden host window. A second launch finds the first by this name.
    static std::wstring host_class(const std::wstring& profile) {
        return profile.empty() ? L"BuddyHostWindow" : L"BuddyHostWindow-" + profile;
    }

    bool start() {
        log::init(paths::data_dir() / L"logs" / L"buddy.log");
        log::info(std::string("Buddy ") + BUDDY_VERSION + " starting");

        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        icon_large_ = win::create_app_icon(GetSystemMetrics(SM_CXICON));
        icon_small_ = win::create_app_icon(GetSystemMetrics(SM_CXSMICON));

        const std::wstring class_name = host_class(profile_);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &App::window_proc;
        wc.hInstance = instance_;
        wc.lpszClassName = class_name.c_str();
        if (!RegisterClassExW(&wc)) return false;
        // A normal (never shown) top-level window, not a message-only one: it must be findable
        // by a second launch and must receive the TaskbarCreated broadcast.
        host_ = CreateWindowExW(0, class_name.c_str(), L"Buddy host", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                instance_, this);
        if (!host_) return false;

        dispatcher_ = std::make_shared<win::UiDispatcher>();
        dispatcher_->attach(host_);
        registrar_.attach(host_);
        transport_ = std::make_unique<WinHttpTransport>(win::widen(std::string("Buddy/") + BUDDY_VERSION));
        credentials_ = std::make_unique<WinCredentialStore>(profile_.empty() ? L"Buddy/" : L"Buddy-" + profile_ + L"/");

        AppController::Deps deps;
        deps.transport = transport_.get();
        deps.credentials = credentials_.get();
        deps.settings_path = paths::data_dir() / L"settings.json";
        deps.theme_folders = {paths::exe_dir() / L"themes", paths::data_dir() / L"themes"};
        deps.app_version = BUDDY_VERSION;
        deps.send_to_ui = [this](const std::string& json) {
            // Every window shows the same state, so every message goes to both.
            if (main_) main_->post(json);
            if (panel_) panel_->post(json);
        };
        deps.run_on_ui = [dispatcher = dispatcher_](std::function<void()> task) { dispatcher->post(std::move(task)); };
        deps.hotkeys = &registrar_;
        deps.hide_window = [this](ViewId view) {
            if (view == ViewId::Panel) {
                if (panel_) panel_->hide(true);
            } else if (main_) {
                main_->hide();
            }
        };
        deps.open_main = [this](bool settings) { open_main(settings); };
        deps.overlay_debug = [this](const protocol::OverlayDebug& command) {
            if (!overlay_) {
                log::warn("overlay.debug ignored: overlay window is not constructed");
                return;
            }
            log::info("overlay.debug dispatch action=" + command.action);
            if (command.action == "clear") {
                overlay_->clear();
            } else if (command.action == "indicator") {
                overlay_->set_indicator(true);
            } else if (command.action == "caption") {
                overlay_->set_caption(win::widen(command.caption));
            } else if (command.action == "demo") {
                overlay_->set_indicator(true);
                overlay_->set_commands({overlay::Arrow{180, 180, 360, 280},
                                        overlay::Highlight{400, 180, 260, 120},
                                        overlay::Pulse{530, 240, 24, 1200}});
            }
        };
        controller_ = std::make_unique<AppController>(std::move(deps));

        WindowServices services;
        services.instance = instance_;
        services.icon = icon_large_;
        services.ui_dir = ui_dir();
        services.user_data_dir = paths::data_dir() / L"WebView2";
#ifndef NDEBUG
        services.developer_mode = true;
#endif
        services.on_ui_message = [this](const std::string& json, ViewId source) {
            if (!controller_) return;
            controller_->handle_ui_message(json, source);
            if (overlay_) {
                const auto& overlay = controller_->settings();
                overlay_->set_palette(overlay.overlay_ring_color, overlay.overlay_dot_color,
                                      overlay.overlay_caption_color);
            }
            sync_shell();   // the message may have changed the theme, the panel options or mute
        };
        services.theme = [this]() -> const theme::Theme& { return controller_->active_theme(); };

        main_ = std::make_unique<MainWindow>(services);
        panel_ = std::make_unique<PanelWindow>(services);
        overlay_ = std::make_unique<OverlayWindow>(instance_, controller_->active_theme());
        if (!overlay_->create()) log::warn("could not create the overlay window");
        const auto& overlay_settings = controller_->settings();
        overlay_->set_palette(overlay_settings.overlay_ring_color, overlay_settings.overlay_dot_color,
                              overlay_settings.overlay_caption_color);
        panel_->on_resized([this](placement::Size size) { controller_->set_panel_size(size.width, size.height); });
        if (!panel_->create()) log::error("could not create the floating panel");

        add_tray_icon();
        const hotkey::Status hotkey_status = controller_->apply_stored_hotkey();
        sync_shell();

        if (hotkey_status != hotkey::Status::Active) {
            // Say so where the user will see it: a notification now, and a banner in the window.
            notify(L"Buddy's shortcut is not working",
                   win::widen(controller_->hotkey_message() + " Choose another shortcut in Buddy's settings."));
        }
        // Normally Buddy starts quietly in the tray. The very first run opens the main window
        // so there is something to see, and so does a start-up without a working hotkey.
        if (controller_->first_run() || hotkey_status != hotkey::Status::Active) {
            open_main(hotkey_status != hotkey::Status::Active);
        }
        log::info(std::string("started; hotkey ") + hotkey::to_code(hotkey_status) + " " + controller_->hotkey_active());
        return true;
    }

    int run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static std::filesystem::path ui_dir() {
#ifndef NDEBUG
        // Debug builds only: BUDDY_UI_DIR points the host at another UI folder.
        wchar_t override_dir[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"BUDDY_UI_DIR", override_dir, MAX_PATH);
        if (length > 0 && length < MAX_PATH) return std::filesystem::path(override_dir);
#endif
        return paths::exe_dir() / L"ui";
    }

    void open_main(bool settings) {
        // An open panel reacts like it does to any other window taking the foreground: it hides,
        // unless it is pinned or click-away hiding is switched off.
        if (main_) {
            main_->show(settings);
            main_->apply_theme();
        }
    }

    // Brings the native side in line with the controller's settings.
    void sync_shell() {
        if (!controller_) return;
        const settings::Settings& s = controller_->settings();
        if (panel_) {
            PanelWindow::Options options;
            options.size_dip = {s.panel_width, s.panel_height};
            options.pinned = s.panel_pinned;
            options.hide_on_click_away = s.hide_on_click_away;
            panel_->set_options(options);
            panel_->apply_theme();
        }
        if (main_) main_->apply_theme();
        update_tray_tip();
    }

    // ---- notification area ----

    NOTIFYICONDATAW tray_data() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = host_;
        data.uID = kTrayIconId;
        return data;
    }

    std::wstring tray_tip() const {
        std::wstring tip = L"Buddy";
        const std::string active = controller_ ? controller_->hotkey_active() : std::string();
        tip += active.empty() ? L" (no shortcut set)" : L" (" + win::widen(active) + L")";
        if (controller_ && controller_->settings().muted) tip += L", muted";
        return tip;
    }

    void add_tray_icon() {
        NOTIFYICONDATAW data = tray_data();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = win::kMsgTray;
        data.hIcon = icon_small_;
        wcsncpy_s(data.szTip, tray_tip().c_str(), _TRUNCATE);
        tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        // Version 4 delivers modern events (keyboard activation, context-menu position).
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        if (!tray_added_) log::warn("could not add the notification-area icon");
    }

    void update_tray_tip() {
        if (!tray_added_) return;
        NOTIFYICONDATAW data = tray_data();
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcsncpy_s(data.szTip, tray_tip().c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void remove_tray_icon() {
        if (!tray_added_) return;
        NOTIFYICONDATAW data = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added_ = false;
    }

    // A notification bubble from the tray icon.
    void notify(const std::wstring& title, const std::wstring& text) {
        if (!tray_added_) return;
        NOTIFYICONDATAW data = tray_data();
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_WARNING;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void show_tray_menu(int x, int y) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open Buddy");
        AppendMenuW(menu, MF_STRING, kCmdSettings, L"Settings");
        AppendMenuW(menu, MF_STRING | (controller_->settings().muted ? MF_CHECKED : 0), kCmdMute, L"Mute");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit");
        SetMenuDefaultItem(menu, kCmdOpen, FALSE);

        // A tray menu only closes on a click elsewhere if its owner is the foreground window.
        SetForegroundWindow(host_);
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, host_, nullptr);
        PostMessageW(host_, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void on_command(UINT command) {
        switch (command) {
            case kCmdOpen: open_main(false); break;
            case kCmdSettings: open_main(true); break;
            case kCmdMute:
                controller_->set_muted(!controller_->settings().muted);
                sync_shell();
                break;
            case kCmdQuit: DestroyWindow(host_); break;
            default: break;
        }
    }

    void quit() {
        log::info("quitting");
        if (controller_) controller_->shutdown();   // also releases the hotkey
        remove_tray_icon();
        panel_.reset();
        main_.reset();
        overlay_.reset();
        dispatcher_->attach(nullptr);
        PostQuitMessage(0);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        App* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->handle(hwnd, message, wparam, lparam);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbar_created_ && taskbar_created_ != 0) {
            // Explorer restarted and forgot every tray icon: put ours back.
            add_tray_icon();
            return 0;
        }
        switch (message) {
            case win::kMsgRunTasks:
                if (dispatcher_) dispatcher_->drain();
                return 0;

            case WM_HOTKEY:
                if (controller_ && panel_ && static_cast<int>(wparam) == controller_->hotkey_id()) panel_->toggle();
                return 0;

            case win::kMsgTray:
                // With NOTIFYICON_VERSION_4 the event is in the low word of lparam and the
                // screen position in wparam.
                switch (LOWORD(lparam)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT: open_main(false); break;
                    case WM_CONTEXTMENU: show_tray_menu(GET_X_LPARAM_(wparam), GET_Y_LPARAM_(wparam)); break;
                    default: break;
                }
                return 0;

            case WM_COMMAND:
                on_command(LOWORD(wparam));
                return 0;

            case win::kMsgActivateInstance:
                open_main(false);   // a second launch of Buddy: show the running one
                return 0;

            case WM_ENDSESSION:
                if (wparam && controller_) controller_->shutdown();
                return 0;

            case WM_DESTROY:
                quit();
                return 0;

            default: break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // Like GET_X_LPARAM / GET_Y_LPARAM, for a position packed into a WPARAM.
    static int GET_X_LPARAM_(WPARAM value) { return static_cast<int>(static_cast<short>(LOWORD(value))); }
    static int GET_Y_LPARAM_(WPARAM value) { return static_cast<int>(static_cast<short>(HIWORD(value))); }

    HINSTANCE instance_;
    std::wstring profile_;
    HWND host_ = nullptr;
    UINT taskbar_created_ = 0;
    HICON icon_large_ = nullptr;
    HICON icon_small_ = nullptr;
    bool tray_added_ = false;

    std::shared_ptr<win::UiDispatcher> dispatcher_;
    WinHotkeyRegistrar registrar_;
    std::unique_ptr<WinHttpTransport> transport_;
    std::unique_ptr<WinCredentialStore> credentials_;
    std::unique_ptr<AppController> controller_;
    std::unique_ptr<MainWindow> main_;
    std::unique_ptr<PanelWindow> panel_;
    std::unique_ptr<OverlayWindow> overlay_;
};

// Debug builds only: BUDDY_PROFILE=<name> gives the run its own data folder, Credential Manager
// entries and single-instance scope, so testing never touches the real settings or keys.
std::wstring debug_profile() {
#ifndef NDEBUG
    wchar_t profile[64] = {};
    const DWORD length = GetEnvironmentVariableW(L"BUDDY_PROFILE", profile, 64);
    if (length > 0 && length < 64) {
        bool valid = true;
        for (DWORD i = 0; i < length; ++i) valid = valid && (iswalnum(profile[i]) || profile[i] == L'-');
        if (valid) return profile;
    }
#endif
    return {};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const std::wstring profile = debug_profile();
    if (!profile.empty()) paths::use_profile(profile);

    // Single instance: the first process owns this named mutex. A later launch finds it taken,
    // asks the first process to show its window, and exits.
    const std::wstring mutex_name = L"Local\\BuddySingleInstance" + (profile.empty() ? L"" : L"-" + profile);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND first = FindWindowW(App::host_class(profile).c_str(), nullptr)) {
            // This new process was started by the user, so it may pass on the right to take the
            // foreground. Without this the first instance's window could only flash.
            DWORD first_pid = 0;
            GetWindowThreadProcessId(first, &first_pid);
            AllowSetForegroundWindow(first_pid);
            PostMessageW(first, win::kMsgActivateInstance, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    // WebView2 is a COM API and needs a single-threaded apartment on the UI thread.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    int exit_code = 1;
    {
        App app(instance, profile);
        if (app.start()) exit_code = app.run();
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return exit_code;
}
