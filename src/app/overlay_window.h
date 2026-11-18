#pragma once

#include <memory>
#include <vector>

#include <windows.h>

#include "core/overlay_commands.h"
#include "core/theme.h"

namespace buddy {

class OverlayWindow {
public:
    OverlayWindow(HINSTANCE instance, const theme::Theme& theme);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool create();
    void destroy();
    void set_commands(std::vector<overlay::Command> commands);
    void clear();
    void set_indicator(bool enabled);
    void set_caption(std::wstring text, DWORD duration_ms = 5000);
    void set_palette(std::string ring, std::string dot, std::string caption);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool create_device();
    void paint();
    void resize();
    void update_indicator();
    void smooth_indicator();
    void update_animation_timer();
    bool has_pulsing_commands() const;

    HINSTANCE instance_;
    const theme::Theme* theme_;
    std::string ring_color_;
    std::string dot_color_;
    std::string caption_color_;
    HWND hwnd_ = nullptr;
    std::vector<overlay::Command> commands_;
    bool indicator_enabled_ = false;
    bool animation_timer_running_ = false;
    POINT indicator_{};
    POINT indicator_target_{};
    bool indicator_positioned_ = false;
    std::wstring caption_;
    ULONGLONG caption_started_ = 0;
    ULONGLONG caption_until_ = 0;
    ULONGLONG caption_collapse_until_ = 0;
    void* factory_ = nullptr;
    void* render_target_ = nullptr;
    void* brush_ = nullptr;
    void* ring_brush_ = nullptr;
    void* dot_brush_ = nullptr;
    void* text_brush_ = nullptr;
    void* text_format_ = nullptr;
};

}  // namespace buddy
