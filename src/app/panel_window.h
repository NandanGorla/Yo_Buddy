#pragma once

#include <functional>
#include <string>

#include <windows.h>

#include "app/main_window.h"   // WindowServices
#include "core/placement.h"

namespace buddy {

// The compact floating panel summoned by the global hotkey.
//
//  - Frameless, always on top, no taskbar button.
//  - Appears next to the mouse cursor on the monitor under it (core/placement.h decides where).
//  - Takes keyboard focus when shown, and gives it back to the window that had it when hidden.
//  - Hides on Esc and, unless pinned or the setting is off, when another window is clicked.
//  - Resizable by its edges, movable by its header; the size is remembered.
//
// The window and its web view are created once at start-up and stay hidden between uses, so
// summoning the panel is instant.
class PanelWindow {
public:
    struct Options {
        placement::Size size_dip = placement::kDefaultPanelDip;
        bool pinned = false;
        bool hide_on_click_away = true;
    };

    explicit PanelWindow(WindowServices services);
    ~PanelWindow();

    PanelWindow(const PanelWindow&) = delete;
    PanelWindow& operator=(const PanelWindow&) = delete;

    bool create();
    void destroy();

    void set_options(const Options& options) { options_ = options; }
    // Called when the user finishes resizing, with the new size in DIPs.
    void on_resized(std::function<void(placement::Size)> callback) { on_resized_ = std::move(callback); }

    void toggle();
    void show_at_cursor();
    void hide(bool restore_focus);

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }
    HWND capture_target_window() const;
    void post(const std::string& json);
    void apply_theme();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT hit_test(LPARAM lparam) const;
    void layout_webview();
    void create_webview();
    bool belongs_to_panel(HWND other) const;

    WindowServices services_;
    Options options_;
    std::function<void(placement::Size)> on_resized_;

    HWND hwnd_ = nullptr;
    HWND previous_foreground_ = nullptr;   // the window to hand focus back to
    HBRUSH background_brush_ = nullptr;
    COLORREF background_ = RGB(255, 255, 255);
    std::string applied_theme_;
    ULONGLONG shown_at_ = 0;
    bool visible_ = false;
    bool hiding_ = false;       // guards against re-entry while focus is being handed back
    bool placing_ = false;      // true while the panel positions itself (ignore DPI suggestions)
    bool page_ready_ = false;
    WebViewHost webview_;
};

}  // namespace buddy
