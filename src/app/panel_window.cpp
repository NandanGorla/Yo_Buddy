#include "app/panel_window.h"

#include <vector>

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include "app/win_util.h"
#include "core/log.h"
#include "core/protocol.h"
#include "platform/capture_target.h"

namespace buddy {
namespace {

constexpr wchar_t kWindowClass[] = L"BuddyPanelWindow";
constexpr int kResizeBorderDip = 6;   // the strip around the web view that resizes the window

placement::Rect to_rect(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

// Collects every monitor's rectangle, work area and DPI for the placement function.
std::vector<placement::Monitor> enumerate_monitors() {
    std::vector<placement::Monitor> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* out = reinterpret_cast<std::vector<placement::Monitor>*>(param);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;
            UINT dpi_x = 96, dpi_y = 96;
            if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
            out->push_back({to_rect(info.rcMonitor), to_rect(info.rcWork), static_cast<int>(dpi_x)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

}  // namespace

PanelWindow::PanelWindow(WindowServices services) : services_(std::move(services)) {}

PanelWindow::~PanelWindow() { destroy(); }

void PanelWindow::post(const std::string& json) {
    if (hwnd_) webview_.post_json(json);
}

HWND PanelWindow::capture_target_window() const {
    if (previous_foreground_ && IsWindow(previous_foreground_)) {
        const HWND root = GetAncestor(previous_foreground_, GA_ROOT);
        const HWND candidate = root ? root : previous_foreground_;
        if (!capture::is_buddy_window(candidate)) return candidate;
    }
    return GetForegroundWindow();
}

bool PanelWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PanelWindow::window_proc;
    wc.hInstance = services_.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = services_.icon;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // WS_POPUP: no title bar. WS_THICKFRAME: Windows still gives it a shadow, rounded corners and
    // edge resizing; WM_NCCALCSIZE below removes the visible frame.
    // WS_EX_TOOLWINDOW: no taskbar button and not in Alt+Tab. WS_EX_TOPMOST: above normal windows.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"Buddy panel",
                            WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0, 440, 560, nullptr, nullptr,
                            services_.instance, this);
    return hwnd_ != nullptr;
}

void PanelWindow::destroy() {
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
    visible_ = false;
}

void PanelWindow::apply_theme() {
    if (!hwnd_ || !services_.theme) return;
    const theme::Theme& theme = services_.theme();
    if (theme.id == applied_theme_) return;
    applied_theme_ = theme.id;

    // The resize strip around the web view is painted in the header colour so it blends in.
    const auto surface = theme.tokens.find("surface");
    background_ = win::color_from_hex(surface != theme.tokens.end() ? surface->second : std::string("#ffffff"),
                                      RGB(255, 255, 255));
    HBRUSH brush = CreateSolidBrush(background_);
    SetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush));
    if (background_brush_) DeleteObject(background_brush_);
    background_brush_ = brush;

    const BOOL dark = theme.kind != "light" ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PanelWindow::create_webview() {
    WebViewConfig config;
    config.parent = hwnd_;
    config.ui_dir = services_.ui_dir;
    config.user_data_dir = services_.user_data_dir;
    config.background = background_;
    config.developer_mode = services_.developer_mode;
    config.start_path = L"/index.html?view=panel";   // the same UI, in its compact layout
    config.start_visible = false;
    config.drag_regions = true;
    webview_.create(
        config,
        [this](std::string json) {
            if (services_.on_ui_message) services_.on_ui_message(json, ViewId::Panel);
            if (!page_ready_) {
                page_ready_ = true;
                if (visible_) {
                    webview_.focus();
                    post(protocol::focus_input());
                }
            }
        },
        [](bool ok, std::string error) {
            if (!ok) log::error("panel webview could not start: " + error);
        },
        [](bool is_error, std::string message) {
            if (is_error) {
                log::error("panel webview: " + message);
            } else {
                log::info("panel webview: " + message);
            }
        });
    layout_webview();
}

void PanelWindow::layout_webview() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int border = win::scale_for(hwnd_, kResizeBorderDip);
    RECT inner{client.left + border, client.top + border, client.right - border, client.bottom - border};
    if (inner.right < inner.left) inner.right = inner.left;
    if (inner.bottom < inner.top) inner.bottom = inner.top;
    webview_.resize(inner);
}

void PanelWindow::toggle() {
    if (visible_) {
        hide(true);
    } else {
        show_at_cursor();
    }
}

void PanelWindow::show_at_cursor() {
    if (!hwnd_) return;

    // Remember who had the keyboard, so it can be handed back on hide.
    const HWND foreground = GetForegroundWindow();
    if (foreground && foreground != hwnd_) {
        const HWND root = GetAncestor(foreground, GA_ROOT);
        const HWND candidate = root ? root : foreground;
        if (!belongs_to_panel(candidate) && !capture::is_buddy_window(candidate)) previous_foreground_ = candidate;
    }

    placement::Input input;
    POINT cursor{};
    GetCursorPos(&cursor);
    input.cursor = {cursor.x, cursor.y};
    input.monitors = enumerate_monitors();
    input.panel_dip = options_.size_dip;
    const placement::Result spot = placement::place_panel(input);

    // Position first without activating, then take the foreground in one step.
    placing_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, spot.rect.left, spot.rect.top, spot.rect.width(), spot.rect.height(),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Moving to a monitor with another DPI can change the size Windows reports; apply it again
    // so the final rectangle is exactly the computed one.
    SetWindowPos(hwnd_, HWND_TOPMOST, spot.rect.left, spot.rect.top, spot.rect.width(), spot.rect.height(),
                 SWP_NOACTIVATE);
    placing_ = false;

    visible_ = true;
    shown_at_ = GetTickCount64();
    webview_.set_visible(true);
    layout_webview();

    const char* method = "";
    const bool foreground_ok = win::bring_to_foreground(hwnd_, &method);
    log::info(std::string("panel shown on monitor ") + std::to_string(spot.monitor) + " at dpi " +
              std::to_string(spot.dpi) + ", foreground: " + method);

    // Put the caret in the message box so typing works straight away.
    if (foreground_ok) SetFocus(hwnd_);
    webview_.focus();
    if (page_ready_) post(protocol::focus_input());
}

void PanelWindow::hide(bool restore_focus) {
    if (!hwnd_ || !visible_ || hiding_) return;
    hiding_ = true;

    const HWND foreground = GetForegroundWindow();
    const bool panel_has_foreground = foreground == hwnd_ || belongs_to_panel(foreground);

    // Hand the keyboard back while this window is still the foreground window: that is when
    // Windows lets a process choose the next foreground window.
    if (restore_focus && panel_has_foreground && previous_foreground_ && IsWindow(previous_foreground_) &&
        IsWindowVisible(previous_foreground_)) {
        const bool ok = SetForegroundWindow(previous_foreground_) != FALSE;
        log::info(std::string("panel hidden, focus handed back: ") + (ok ? "yes" : "refused"));
    }
    ShowWindow(hwnd_, SW_HIDE);
    webview_.set_visible(false);
    visible_ = false;
    previous_foreground_ = nullptr;
    hiding_ = false;
}

// True for the panel itself and for windows it owns or contains (WebView2's own child and popup
// windows live in another process but hang off this window).
bool PanelWindow::belongs_to_panel(HWND other) const {
    if (!other || !hwnd_) return false;
    if (other == hwnd_ || IsChild(hwnd_, other)) return true;
    return GetAncestor(other, GA_ROOTOWNER) == hwnd_;
}

LRESULT PanelWindow::hit_test(LPARAM lparam) const {
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);
    const int border = win::scale_for(hwnd_, kResizeBorderDip);
    // Corners get a slightly larger grab area than edges.
    const int corner = border * 2;

    const bool left = x < window.left + border;
    const bool right = x >= window.right - border;
    const bool top = y < window.top + border;
    const bool bottom = y >= window.bottom - border;
    const bool near_left = x < window.left + corner;
    const bool near_right = x >= window.right - corner;
    const bool near_top = y < window.top + corner;
    const bool near_bottom = y >= window.bottom - corner;

    if ((top && near_left) || (left && near_top)) return HTTOPLEFT;
    if ((top && near_right) || (right && near_top)) return HTTOPRIGHT;
    if ((bottom && near_left) || (left && near_bottom)) return HTBOTTOMLEFT;
    if ((bottom && near_right) || (right && near_bottom)) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
}

LRESULT CALLBACK PanelWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    PanelWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<PanelWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PanelWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && self->hwnd_ == hwnd) return self->handle(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT PanelWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            apply_theme();
            // Rounded corners from the window manager (Windows 11; ignored on Windows 10).
            const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            create_webview();
            return 0;
        }

        case WM_NCCALCSIZE:
            // Returning 0 for wparam == TRUE makes the client area cover the whole window, which
            // removes the standard frame while keeping the WS_THICKFRAME behaviours.
            if (wparam == TRUE) return 0;
            break;

        case WM_NCHITTEST: return hit_test(lparam);

        case WM_SIZE:
            layout_webview();
            return 0;

        case WM_MOVE:
        case WM_MOVING: webview_.notify_parent_moved(); break;

        case WM_EXITSIZEMOVE: {
            // The user finished dragging or resizing: remember the size in DIPs.
            RECT window{};
            GetWindowRect(hwnd_, &window);
            const int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
            const placement::Size size{placement::unscale(window.right - window.left, dpi),
                                       placement::unscale(window.bottom - window.top, dpi)};
            options_.size_dip = placement::clamp_panel_size(size);
            if (on_resized_) on_resized_(options_.size_dip);
            return 0;
        }

        case WM_DPICHANGED: {
            if (placing_) return 0;   // the panel is positioning itself with an exact rectangle
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = win::scale_for(hwnd_, placement::kMinPanelDip.width);
            info->ptMinTrackSize.y = win::scale_for(hwnd_, placement::kMinPanelDip.height);
            return 0;
        }

        case WM_SETFOCUS:
            webview_.focus();
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                const HWND next = reinterpret_cast<HWND>(lparam);
                if (next) {
                    const HWND root = GetAncestor(next, GA_ROOT);
                    const HWND candidate = root ? root : next;
                    if (!belongs_to_panel(candidate) && !capture::is_buddy_window(candidate)) {
                        previous_foreground_ = candidate;
                    }
                }
            }
            // Click-away: another window was activated. Ignore the panel's own popups, the first
            // moments after showing, and the deactivation the panel causes itself while hiding.
            if (LOWORD(wparam) == WA_INACTIVE && visible_ && !hiding_ && !options_.pinned &&
                options_.hide_on_click_away && !belongs_to_panel(reinterpret_cast<HWND>(lparam)) &&
                GetTickCount64() - shown_at_ > 200) {
                hide(false);   // the user already chose where focus goes
            }
            break;

        case WM_CLOSE:
            hide(true);   // Alt+F4 hides the panel; Quit is in the tray menu
            return 0;

        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace buddy
