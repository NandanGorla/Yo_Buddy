#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <windows.h>

#include "app/app_controller.h"
#include "core/theme.h"
#include "webhost/webview_host.h"

namespace buddy {

// What a window needs from the application that owns it.
struct WindowServices {
    HINSTANCE instance = nullptr;
    HICON icon = nullptr;
    std::filesystem::path ui_dir;
    std::filesystem::path user_data_dir;
    bool developer_mode = false;
    // Called with every message the window's web UI posts.
    std::function<void(const std::string& json, ViewId source)> on_ui_message;
    // The theme to dress the native parts of the window in.
    std::function<const theme::Theme&()> theme;
};

// The full Buddy window: chat plus the settings page. It is created the first time it is
// opened. Closing it only hides it; the app keeps running in the notification area.
class MainWindow {
public:
    explicit MainWindow(WindowServices services);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void show(bool open_settings);   // creates the window if needed and brings it to the front
    void hide();
    void destroy();

    bool visible() const;
    HWND hwnd() const { return hwnd_; }
    HWND capture_target_window() const;
    void post(const std::string& json);   // one protocol message to this window's web UI
    void apply_theme();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    bool create();
    void create_webview();

    WindowServices services_;
    HWND hwnd_ = nullptr;
    HWND previous_foreground_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    COLORREF background_ = RGB(255, 255, 255);
    std::string applied_theme_;
    bool page_ready_ = false;
    bool pending_settings_ = false;
    WebViewHost webview_;
};

}  // namespace buddy
