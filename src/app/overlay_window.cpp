#include "app/overlay_window.h"

#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dwmapi.h>
#include <type_traits>
#include <utility>
#include <wrl/client.h>

#include "app/win_util.h"
#include "core/log.h"
#include "platform/win_text.h"

namespace buddy {
namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t kClassName[] = L"BuddyOverlayWindow";
constexpr COLORREF kTransparentKey = RGB(0, 0, 0);
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 16;
constexpr LONG kIndicatorOffsetPixels = 24;
constexpr ULONGLONG kIndicatorBlinkIntervalMs = 500;
constexpr ULONGLONG kCaptionExpandDurationMs = 350;

template <typename T>
T* as(void* value) {
    return static_cast<T*>(value);
}

void draw_arrow(ID2D1HwndRenderTarget* target, ID2D1Factory* factory, ID2D1Brush* brush,
                D2D1_POINT_2F start, D2D1_POINT_2F end) {
    target->DrawLine(start, end, brush, 3.0f);
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;

    const float ux = dx / length;
    const float uy = dy / length;
    const float head = std::min(18.0f, length * 0.4f);
    const float half = head * 0.42f;
    const D2D1_POINT_2F base{end.x - ux * head, end.y - uy * head};
    const D2D1_POINT_2F left{base.x - uy * half, base.y + ux * half};
    const D2D1_POINT_2F right{base.x + uy * half, base.y - ux * half};

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(end, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(left);
    sink->AddLine(right);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close())) target->FillGeometry(geometry.Get(), brush);
}

}  // namespace

OverlayWindow::OverlayWindow(HINSTANCE instance, const theme::Theme& theme)
    : instance_(instance),
      theme_(&theme),
      ring_color_(theme.overlay.count("ring") ? theme.overlay.at("ring") : "#ffb000"),
      dot_color_(theme.overlay.count("dot") ? theme.overlay.at("dot") : "#ffffff"),
      caption_color_(theme.overlay.count("caption-text") ? theme.overlay.at("caption-text") : "#ffffff") {}

OverlayWindow::~OverlayWindow() { destroy(); }

bool OverlayWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OverlayWindow::window_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            log::error("overlay: RegisterClassExW failed with error " + std::to_string(error));
            return false;
        }
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kClassName, L"Buddy overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        log::error("overlay: CreateWindowExW failed with error " + std::to_string(GetLastError()));
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, kTransparentKey, 0, LWA_COLORKEY);
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        log::warn("overlay: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed with error " +
                  std::to_string(GetLastError()));
    }
    const bool ready = create_device();
    log::info(std::string("overlay: created hwnd=") +
              std::to_string(reinterpret_cast<std::uintptr_t>(hwnd_)) +
              " render_ready=" + (ready ? "true" : "false"));
    return ready;
}

bool OverlayWindow::create_device() {
    ComPtr<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return false;
    factory_ = factory.Detach();
    resize();
    return render_target_ != nullptr;
}

void OverlayWindow::resize() {
    if (!hwnd_ || !factory_) return;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    if (render_target_) {
        as<ID2D1HwndRenderTarget>(render_target_)->Resize(
            D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
        return;
    }
    ComPtr<ID2D1HwndRenderTarget> target;
    const auto props = D2D1::RenderTargetProperties();
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
    if (FAILED(as<ID2D1Factory>(factory_)->CreateHwndRenderTarget(props, hwnd_props, &target))) return;
    render_target_ = target.Detach();
    // Indicator and drawing coordinates are physical pixels, matching GetCursorPos and
    // the per-monitor-aware overlay window. Do not let Direct2D scale them with monitor DPI.
    as<ID2D1HwndRenderTarget>(render_target_)->SetDpi(96.0f, 96.0f);
    ComPtr<ID2D1SolidColorBrush> brush;
    const auto highlight = theme_->overlay.find("highlight");
    const COLORREF color = win::color_from_hex(highlight == theme_->overlay.end() ? "#ffb000" : highlight->second,
                                               RGB(255, 176, 0));
    const auto d2d_color = D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                                        GetBValue(color) / 255.0f);
    if (FAILED(as<ID2D1HwndRenderTarget>(render_target_)->CreateSolidColorBrush(d2d_color, &brush))) return;
    brush_ = brush.Detach();
    const auto make_brush = [&](const char* name, COLORREF fallback, void** output) {
        const std::string& configured = std::strcmp(name, "ring") == 0
                                            ? ring_color_
                                            : (std::strcmp(name, "dot") == 0 ? dot_color_ : caption_color_);
        const COLORREF value = win::color_from_hex(configured, fallback);
        const auto color_value = D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
                                              GetBValue(value) / 255.0f);
        ComPtr<ID2D1SolidColorBrush> result;
        if (FAILED(as<ID2D1HwndRenderTarget>(render_target_)->CreateSolidColorBrush(color_value, &result))) {
            return false;
        }
        *output = result.Detach();
        return true;
    };
    if (!make_brush("ring", color, &ring_brush_) || !make_brush("dot", RGB(255, 255, 255), &dot_brush_) ||
        !make_brush("caption-text", RGB(255, 255, 255), &text_brush_)) {
        return;
    }
    ComPtr<IDWriteFactory> write_factory;
    ComPtr<IDWriteTextFormat> text_format;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(write_factory.GetAddressOf()))) &&
        SUCCEEDED(write_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
                                                  L"", &text_format))) {
        text_format_ = text_format.Detach();
    }
}

void OverlayWindow::paint() {
    if (!render_target_ || !brush_) return;
    auto* target = as<ID2D1HwndRenderTarget>(render_target_);
    auto* brush = as<ID2D1SolidColorBrush>(brush_);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const ULONGLONG now = GetTickCount64();
    for (const auto& command : commands_) {
        std::visit(
            [target, brush, now, text_format = text_format_, factory = factory_](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, overlay::Arrow>) {
                    draw_arrow(target, as<ID2D1Factory>(factory), brush,
                               D2D1::Point2F(static_cast<float>(item.x1), static_cast<float>(item.y1)),
                               D2D1::Point2F(static_cast<float>(item.x2), static_cast<float>(item.y2)));
                } else if constexpr (std::is_same_v<T, overlay::Circle>) {
                    target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(static_cast<float>(item.x), static_cast<float>(item.y)),
                                                      static_cast<float>(item.radius), static_cast<float>(item.radius)),
                                        brush, 3.0f);
                } else if constexpr (std::is_same_v<T, overlay::Highlight>) {
                    target->DrawRectangle(D2D1::RectF(static_cast<float>(item.x), static_cast<float>(item.y),
                                                       static_cast<float>(item.x + item.width),
                                                       static_cast<float>(item.y + item.height)),
                                          brush, 3.0f);
                } else if constexpr (std::is_same_v<T, overlay::Pulse>) {
                    const float phase = static_cast<float>((now % item.duration_ms) / static_cast<double>(item.duration_ms));
                    const float radius = static_cast<float>(item.radius) * (1.0f + phase);
                    target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(static_cast<float>(item.x), static_cast<float>(item.y)),
                                                      radius, radius),
                                        brush, 3.0f);
                } else if constexpr (std::is_same_v<T, overlay::Label>) {
                    const auto rect = D2D1::RectF(static_cast<float>(item.x), static_cast<float>(item.y),
                                                  static_cast<float>(item.x + 500), static_cast<float>(item.y + 80));
                    if (text_format) {
                        const std::wstring text = win::widen(item.text);
                        target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()),
                                          as<IDWriteTextFormat>(text_format), rect, brush);
                    }
                }
            },
            command);
    }
    const bool caption_visible = !caption_.empty() && now < caption_collapse_until_;
    if (indicator_enabled_ || caption_visible) {
        const auto center = D2D1::Point2F(static_cast<float>(indicator_.x), static_cast<float>(indicator_.y));
        const bool showing_caption = !caption_.empty() && now < caption_until_;
        float radius = 9.0f;
        float capsule_width = 0.0f;
        if (showing_caption) {
            const float progress = std::min(
                1.0f, static_cast<float>(now - caption_started_) / static_cast<float>(kCaptionExpandDurationMs));
            radius = 9.0f + 5.0f * progress;
            capsule_width = (24.0f + static_cast<float>(caption_.size()) * 8.0f) * progress;
        } else if (caption_visible) {
            const float progress = 1.0f - std::min(
                1.0f, static_cast<float>(now - caption_until_) / static_cast<float>(kCaptionExpandDurationMs));
            radius = 9.0f + 5.0f * progress;
            capsule_width = (24.0f + static_cast<float>(caption_.size()) * 8.0f) * progress;
        }
        if (capsule_width > 0.0f) {
            const auto capsule = D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(indicator_.x) - radius,
                            static_cast<float>(indicator_.y) - radius,
                            static_cast<float>(indicator_.x) + capsule_width,
                            static_cast<float>(indicator_.y) + radius),
                radius, radius);
            target->DrawRoundedRectangle(capsule, as<ID2D1Brush>(ring_brush_), 3.0f);
            if (showing_caption && text_format_ && capsule_width > 8.0f) {
                const auto rect = D2D1::RectF(static_cast<float>(indicator_.x + 12),
                                              static_cast<float>(indicator_.y - radius + 2),
                                              static_cast<float>(indicator_.x + capsule_width - 8),
                                              static_cast<float>(indicator_.y + radius - 2));
                target->DrawTextW(caption_.c_str(), static_cast<UINT32>(caption_.size()),
                                  as<IDWriteTextFormat>(text_format_), rect, as<ID2D1Brush>(text_brush_));
            }
        } else {
            target->DrawEllipse(D2D1::Ellipse(center, radius, radius), as<ID2D1Brush>(ring_brush_), 3.0f);
        }
        if ((now / kIndicatorBlinkIntervalMs) % 2 == 0) {
            target->FillEllipse(D2D1::Ellipse(center, 3.0f, 3.0f), as<ID2D1Brush>(dot_brush_));
        }
    }
    target->EndDraw();
}

void OverlayWindow::set_commands(std::vector<overlay::Command> commands) {
    commands_ = std::move(commands);
    if (hwnd_) {
        if (commands_.empty() && !indicator_enabled_ && GetTickCount64() >= caption_collapse_until_) {
            caption_.clear();
        }
        if (!commands_.empty()) {
            update_indicator();
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        } else if (!indicator_enabled_ && caption_.empty()) {
            ShowWindow(hwnd_, SW_HIDE);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        update_animation_timer();
    }
}

void OverlayWindow::clear() {
    commands_.clear();
    indicator_enabled_ = false;
    caption_.clear();
    caption_started_ = 0;
    caption_until_ = 0;
    caption_collapse_until_ = 0;
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        update_animation_timer();
    }
    log::info("overlay: cleared");
}

void OverlayWindow::set_indicator(bool enabled) {
    indicator_enabled_ = enabled;
    log::info(std::string("overlay: indicator ") + (enabled ? "enabled" : "disabled"));
    if (hwnd_) {
        if (!enabled && GetTickCount64() >= caption_collapse_until_) caption_.clear();
        if (enabled) update_indicator();
        else if (commands_.empty() && caption_.empty()) ShowWindow(hwnd_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        update_animation_timer();
    }
}

void OverlayWindow::set_caption(std::wstring text, DWORD /*duration_ms*/) {
    caption_ = std::move(text);
    if (caption_.empty()) {
        caption_started_ = caption_until_ = caption_collapse_until_ = 0;
        if (hwnd_) {
            if (!indicator_enabled_ && commands_.empty()) ShowWindow(hwnd_, SW_HIDE);
            InvalidateRect(hwnd_, nullptr, FALSE);
            update_animation_timer();
        }
        return;
    }
    caption_started_ = GetTickCount64();
    const DWORD length_duration = 4000 + static_cast<DWORD>(std::min<std::size_t>(caption_.size() * 80, 4000));
    caption_until_ = caption_started_ + std::clamp<DWORD>(length_duration, DWORD{4000}, DWORD{8000});
    caption_collapse_until_ = caption_until_ + kCaptionExpandDurationMs;
    if (hwnd_) {
        update_indicator();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        update_animation_timer();
    }
}

bool OverlayWindow::has_pulsing_commands() const {
    return std::any_of(commands_.begin(), commands_.end(),
                       [](const overlay::Command& command) { return std::holds_alternative<overlay::Pulse>(command); });
}

void OverlayWindow::update_animation_timer() {
    if (!hwnd_) return;
    const ULONGLONG now = GetTickCount64();
    const bool caption_animating = !caption_.empty() && now < caption_collapse_until_;
    const bool needed = indicator_enabled_ || caption_animating || has_pulsing_commands();
    if (needed && !animation_timer_running_) {
        animation_timer_running_ = SetTimer(hwnd_, kAnimationTimer, kAnimationIntervalMs, nullptr) != 0;
        if (!animation_timer_running_) log::warn("overlay: could not start the animation timer");
    } else if (!needed && animation_timer_running_) {
        KillTimer(hwnd_, kAnimationTimer);
        animation_timer_running_ = false;
    }
}

void OverlayWindow::set_palette(std::string ring, std::string dot, std::string caption) {
    if (ring_color_ == ring && dot_color_ == dot && caption_color_ == caption) return;
    ring_color_ = std::move(ring);
    dot_color_ = std::move(dot);
    caption_color_ = std::move(caption);
    if (!hwnd_ || !render_target_) return;
    if (brush_) as<ID2D1SolidColorBrush>(brush_)->Release();
    if (ring_brush_) as<ID2D1SolidColorBrush>(ring_brush_)->Release();
    if (dot_brush_) as<ID2D1SolidColorBrush>(dot_brush_)->Release();
    if (text_brush_) as<ID2D1SolidColorBrush>(text_brush_)->Release();
    as<ID2D1HwndRenderTarget>(render_target_)->Release();
    brush_ = ring_brush_ = dot_brush_ = text_brush_ = render_target_ = nullptr;
    resize();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::update_indicator() {
    if (!hwnd_) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        log::warn("overlay: GetCursorPos failed");
        return;
    }
    RECT work{};
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) work = info.rcWork;
    indicator_target_.x = cursor.x - work.left + kIndicatorOffsetPixels;
    indicator_target_.y = cursor.y - work.top + kIndicatorOffsetPixels;
    if (!indicator_positioned_) {
        indicator_ = indicator_target_;
        indicator_positioned_ = true;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, work.left, work.top, work.right - work.left, work.bottom - work.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OverlayWindow::smooth_indicator() {
    if (!indicator_positioned_) return;
    const auto smooth = [](LONG& current, LONG target) {
        const LONG delta = target - current;
        if (delta == 0) return;
        const LONG step = delta / 2;
        current += step == 0 ? (delta > 0 ? 1 : -1) : step;
    };
    smooth(indicator_.x, indicator_target_.x);
    smooth(indicator_.y, indicator_target_.y);
}

void OverlayWindow::destroy() {
    if (hwnd_ && animation_timer_running_) KillTimer(hwnd_, kAnimationTimer);
    animation_timer_running_ = false;
    if (brush_) as<ID2D1SolidColorBrush>(brush_)->Release();
    if (ring_brush_) as<ID2D1SolidColorBrush>(ring_brush_)->Release();
    if (dot_brush_) as<ID2D1SolidColorBrush>(dot_brush_)->Release();
    if (text_brush_) as<ID2D1SolidColorBrush>(text_brush_)->Release();
    if (render_target_) as<ID2D1HwndRenderTarget>(render_target_)->Release();
    if (factory_) as<ID2D1Factory>(factory_)->Release();
    if (text_format_) as<IDWriteTextFormat>(text_format_)->Release();
    brush_ = ring_brush_ = dot_brush_ = text_brush_ = render_target_ = factory_ = text_format_ = nullptr;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK OverlayWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT OverlayWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint_struct{};
            BeginPaint(hwnd_, &paint_struct);
            paint();
            EndPaint(hwnd_, &paint_struct);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: resize(); return 0;
        case WM_TIMER:
            if (wparam == kAnimationTimer) {
                const ULONGLONG now = GetTickCount64();
                const bool caption_animating = !caption_.empty() && now < caption_collapse_until_;
                if (indicator_enabled_ || caption_animating) {
                    update_indicator();
                    smooth_indicator();
                }
                if (!caption_animating && !indicator_enabled_) {
                    caption_.clear();
                    if (commands_.empty()) ShowWindow(hwnd_, SW_HIDE);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                update_animation_timer();
            }
            return 0;
        case WM_NCHITTEST: return HTTRANSPARENT;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace buddy
