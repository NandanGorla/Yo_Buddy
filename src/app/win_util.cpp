#include "app/win_util.h"

#include <cstdint>
#include <vector>

namespace buddy::win {

COLORREF color_from_hex(const std::string& hex, COLORREF fallback) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() == 4 && hex[0] == '#') {
        const int r = digit(hex[1]), g = digit(hex[2]), b = digit(hex[3]);
        if (r < 0 || g < 0 || b < 0) return fallback;
        return RGB(r * 17, g * 17, b * 17);
    }
    if ((hex.size() == 7 || hex.size() == 9) && hex[0] == '#') {
        int v[6];
        for (int i = 0; i < 6; ++i) {
            v[i] = digit(hex[static_cast<std::size_t>(1 + i)]);
            if (v[i] < 0) return fallback;
        }
        return RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
    }
    return fallback;
}

int scale_for(HWND hwnd, int dip) { return MulDiv(dip, static_cast<int>(GetDpiForWindow(hwnd)), 96); }

void UiDispatcher::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    if (hwnd_) PostMessageW(hwnd_, kMsgRunTasks, 0, 0);
}

void UiDispatcher::drain() {
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(tasks_);
    }
    for (auto& task : batch) task();
}

bool bring_to_foreground(HWND hwnd, const char** method) {
    const auto report = [&](const char* how, bool ok) {
        if (method) *method = how;
        return ok;
    };
    if (GetForegroundWindow() == hwnd) return report("already", true);

    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return report("direct", true);

    // Windows refused. Borrow the foreground thread's input state and try again.
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD this_thread = GetCurrentThreadId();
    if (foreground_thread != 0 && foreground_thread != this_thread &&
        AttachThreadInput(this_thread, foreground_thread, TRUE)) {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        AttachThreadInput(this_thread, foreground_thread, FALSE);
        if (GetForegroundWindow() == hwnd) return report("attach", true);
    }
    return report("refused", false);
}

HICON create_app_icon(int size_px) {
    if (size_px < 16) size_px = 16;

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size_px;
    header.bV5Height = -size_px;   // top-down rows
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!color || !dc || !bits) {
        if (color) DeleteObject(color);
        if (dc) DeleteDC(dc);
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    HGDIOBJ old_bitmap = SelectObject(dc, color);
    const COLORREF accent = RGB(0x24, 0x56, 0xD6);
    RECT all{0, 0, size_px, size_px};
    HBRUSH brush = CreateSolidBrush(accent);
    FillRect(dc, &all, brush);
    DeleteObject(brush);

    HFONT font = CreateFontW(-MulDiv(size_px, 62, 100), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"B", 1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    DeleteObject(font);
    GdiFlush();

    // GDI leaves the alpha byte at zero. Set it: opaque inside a rounded square, clear outside.
    auto* pixels = static_cast<std::uint32_t*>(bits);
    const double radius = size_px * 0.22;
    for (int y = 0; y < size_px; ++y) {
        for (int x = 0; x < size_px; ++x) {
            // Distance from the pixel centre to the nearest corner circle centre, when in a corner.
            const double px = x + 0.5, py = y + 0.5;
            const double cx = px < radius ? radius : (px > size_px - radius ? size_px - radius : px);
            const double cy = py < radius ? radius : (py > size_px - radius ? size_px - radius : py);
            const double dx = px - cx, dy = py - cy;
            const bool inside = dx * dx + dy * dy <= radius * radius;
            std::uint32_t& p = pixels[y * size_px + x];
            p = inside ? (p | 0xFF000000u) : 0u;
        }
    }
    SelectObject(dc, old_bitmap);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size_px, size_px, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

}  // namespace buddy::win
