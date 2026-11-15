#include "app/main_window.h"

#include <dwmapi.h>

#include "app/win_util.h"
#include "core/log.h"
#include "core/protocol.h"
#include "platform/win_text.h"

namespace buddy {
namespace {

constexpr wchar_t kWindowClass[] = L"BuddyMainWindow";
constexpr int kDefaultWidthDip = 1040;   // sizes in device-independent pixels (96 per inch)
constexpr int kDefaultHeightDip = 760;
constexpr int kMinWidthDip = 520;
constexpr int kMinHeightDip = 420;

}  // namespace

MainWindow::MainWindow(WindowServices services) : services_(std::move(services)) {}

MainWindow::~MainWindow() { destroy(); }

bool MainWindow::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void MainWindow::post(const std::string& json) {
    if (hwnd_) webview_.post_json(json);
}

bool MainWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::window_proc;
    wc.hInstance = services_.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = services_.icon;
    wc.hIconSm = services_.icon;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);   // fails harmlessly if the class already exists

    // Created at 96 DPI sizes and corrected in WM_CREATE once the window's DPI is known.
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Buddy", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            kDefaultWidthDip, kDefaultHeightDip, nullptr, nullptr, services_.instance, this);
    return hwnd_ != nullptr;
}

void MainWindow::show(bool open_settings) {
    if (!hwnd_ && !create()) {
        log::error("could not create the main window");
        return;
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    webview_.set_visible(true);
    const char* method = "";
    win::bring_to_foreground(hwnd_, &method);
    log::info(std::string("main window shown, foreground: ") + method);
    webview_.focus();

    if (open_settings) {
        if (page_ready_) {
            post(protocol::show_settings());
        } else {
            pending_settings_ = true;   // sent as soon as the page has loaded
        }
    } else if (page_ready_) {
        post(protocol::focus_input());   // opened to chat: put the caret in the message box
    }
}

void MainWindow::hide() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    webview_.set_visible(false);   // lets WebView2 stop rendering while hidden
}

void MainWindow::destroy() {
    webview_.close();
    if (hwnd_) {
        HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
    if (background_brush_) {
        DeleteObject(background_brush_);
        background_brush_ = nullptr;
    }
}

void MainWindow::apply_theme() {
    if (!hwnd_ || !services_.theme) return;
    const theme::Theme& theme = services_.theme();
    if (theme.id == applied_theme_) return;
    applied_theme_ = theme.id;

    const auto bg = theme.tokens.find("bg");
    background_ = win::color_from_hex(bg != theme.tokens.end() ? bg->second : std::string("#ffffff"), RGB(255, 255, 255));

    // Title bar colour follows light/dark (supported from Windows 10 2004 / Windows 11).
    const BOOL dark = theme.kind != "light" ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    HBRUSH brush = CreateSolidBrush(background_);
    SetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush));
    if (background_brush_) DeleteObject(background_brush_);
    background_brush_ = brush;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::create_webview() {
    WebViewConfig config;
    config.parent = hwnd_;
    config.ui_dir = services_.ui_dir;
    config.user_data_dir = services_.user_data_dir;
    config.background = background_;
    config.developer_mode = services_.developer_mode;
    config.start_path = L"/index.html";
    webview_.create(
        config,
        [this](std::string json) {
            if (services_.on_ui_message) services_.on_ui_message(json, ViewId::Main);
            // The first message means the page has loaded and can take focus and commands.
            if (!page_ready_) {
                page_ready_ = true;
                if (GetForegroundWindow() == hwnd_) webview_.focus();
                post(protocol::focus_input());
                if (pending_settings_) {
                    pending_settings_ = false;
                    post(protocol::show_settings());
                }
            }
        },
        [this](bool ok, std::string error) {
            if (ok) return;
            const std::wstring text =
                L"Buddy could not start its user interface.\n\n" + win::widen(error) +
                L"\n\nBuddy needs the Microsoft Edge WebView2 Runtime, which is part of Windows 11 "
                L"and can be installed from Microsoft on Windows 10.";
            MessageBoxW(hwnd_, text.c_str(), L"Buddy", MB_OK | MB_ICONERROR);
        },
        [](bool is_error, std::string message) {
            if (is_error) {
                log::error("main webview: " + message);
            } else {
                log::info("main webview: " + message);
            }
        });
}

LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && self->hwnd_ == hwnd) return self->handle(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT MainWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            apply_theme();
            // Now that the window has a DPI, give it its real default size.
            SetWindowPos(hwnd_, nullptr, 0, 0, win::scale_for(hwnd_, kDefaultWidthDip),
                         win::scale_for(hwnd_, kDefaultHeightDip), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            create_webview();
            return 0;

        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                webview_.set_visible(false);
            } else {
                RECT client{};
                GetClientRect(hwnd_, &client);
                webview_.set_visible(true);
                webview_.resize(client);
            }
            return 0;

        case WM_MOVE:
        case WM_MOVING: webview_.notify_parent_moved(); break;

        case WM_DPICHANGED: {
            // Windows suggests a rectangle that keeps the window the same physical size on the
            // new monitor. WebView2 picks up the new scale on its own.
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = win::scale_for(hwnd_, kMinWidthDip);
            info->ptMinTrackSize.y = win::scale_for(hwnd_, kMinHeightDip);
            return 0;
        }

        case WM_SETFOCUS:
            webview_.focus();   // the host window itself has nothing to focus
            return 0;

        case WM_CLOSE:
            hide();   // closing only hides: Buddy keeps running in the notification area
            return 0;

        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace buddy
