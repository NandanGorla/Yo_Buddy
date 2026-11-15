#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

namespace buddy {

struct WebViewConfig {
    HWND parent = nullptr;
    std::filesystem::path ui_dir;          // folder holding index.html; served via virtual host mapping
    std::filesystem::path user_data_dir;   // WebView2 profile folder (cache, local storage)
    bool developer_mode = false;           // DevTools, context menu and browser shortcuts (debug builds)
    COLORREF background = RGB(255, 255, 255);   // shown before the page paints, avoids a white flash
    std::wstring start_path = L"/index.html";   // page to load, may carry a query such as ?view=panel
    bool start_visible = true;             // false for a window that starts hidden
    bool drag_regions = false;             // honour CSS "app-region: drag" (frameless windows)
};

// Owns the WebView2 control that renders the whole UI, and locks it down (STATUS.md D17):
//
//  - The UI is loaded from https://app.buddy.example/, a virtual host mapped to `ui_dir`.
//  - Every request to any other http(s)/ws/file URL is answered with 403 by the host.
//  - Navigating away, opening new windows, downloads and external URI schemes are cancelled.
//  - Every permission request (camera, microphone, location, clipboard read, ...) is denied.
//  - Web messages are accepted only when they come from the bundled origin's top-level document.
//  - Outside developer mode, DevTools, the default context menu and browser shortcuts are off.
//
// All methods must be called on the UI thread. Callbacks are also invoked on the UI thread.
class WebViewHost {
public:
    using MessageHandler = std::function<void(std::string json_utf8)>;
    using ReadyHandler = std::function<void(bool ok, std::string error)>;
    using LogHandler = std::function<void(bool is_error, std::string message)>;

    static constexpr const wchar_t* kHostName = L"app.buddy.example";
    static constexpr const wchar_t* kOrigin = L"https://app.buddy.example";

    WebViewHost();
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    // Starts asynchronous creation. `on_ready` reports the outcome exactly once.
    void create(const WebViewConfig& config, MessageHandler on_message, ReadyHandler on_ready, LogHandler log);

    void resize(const RECT& bounds_in_parent);
    void set_visible(bool visible);
    void focus();                    // moves keyboard focus into the web content
    void notify_parent_moved();      // keeps popups (select lists, tooltips) attached to the window
    void post_json(std::string_view json_utf8);
    void close();

    bool ready() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace buddy
