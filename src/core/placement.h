#pragma once

#include <vector>

// Where the floating panel goes when it is summoned. This is pure arithmetic on numbers the
// window code collects from Windows (cursor position, monitor rectangles, DPI), so it can be
// unit-tested without a screen.
//
// Units: everything is in physical pixels of the virtual desktop, except sizes named *_dip,
// which are device-independent pixels (96 per inch) and are scaled by the target monitor's DPI.
namespace buddy::placement {

struct Point {
    int x = 0;
    int y = 0;
};

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;    // exclusive
    int bottom = 0;   // exclusive

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool contains(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
    bool operator==(const Rect&) const = default;
};

struct Monitor {
    Rect bounds;     // the whole monitor
    Rect work;       // the monitor minus the taskbar and other docked bars
    int dpi = 96;    // effective DPI: 96 = 100 %, 144 = 150 %
};

inline constexpr Size kDefaultPanelDip{440, 560};
inline constexpr Size kMinPanelDip{320, 240};
inline constexpr Size kMaxPanelDip{1600, 1400};

struct Input {
    Point cursor;
    std::vector<Monitor> monitors;
    Size panel_dip = kDefaultPanelDip;
    int gap_dip = 14;      // distance between the cursor and the panel's nearest corner
    int margin_dip = 8;    // breathing room kept between the panel and the work-area edge
};

struct Result {
    Rect rect;          // where to put the panel
    int dpi = 96;       // DPI of the monitor it lands on
    int monitor = -1;   // index into Input::monitors, or -1 when there were none
};

// Rules, in order:
//  1. The panel goes on the monitor under the cursor (or the nearest one if the cursor is
//     somehow outside all of them).
//  2. Its size is the stored size scaled to that monitor's DPI, shrunk if the work area is smaller.
//  3. It prefers the spot below and to the right of the cursor. If it would not fit there it
//     flips to the left and/or above, so it never covers the cursor.
//  4. Finally it is clamped to the work area, so it is always fully visible.
Result place_panel(const Input& input);

int scale(int dip, int dpi);      // DIPs to pixels, rounded
int unscale(int pixels, int dpi); // pixels to DIPs, rounded
Size clamp_panel_size(Size dip);  // keeps a stored size within the allowed range

}  // namespace buddy::placement
