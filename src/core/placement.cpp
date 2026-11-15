#include "core/placement.h"

#include <algorithm>
#include <cstdint>

namespace buddy::placement {
namespace {

// Squared distance from a point to a rectangle (0 when inside).
std::int64_t distance_squared(Point p, const Rect& r) {
    const std::int64_t dx = p.x < r.left ? r.left - p.x : (p.x >= r.right ? p.x - (r.right - 1) : 0);
    const std::int64_t dy = p.y < r.top ? r.top - p.y : (p.y >= r.bottom ? p.y - (r.bottom - 1) : 0);
    return dx * dx + dy * dy;
}

}  // namespace

int scale(int dip, int dpi) {
    if (dpi <= 0) dpi = 96;
    return static_cast<int>((static_cast<std::int64_t>(dip) * dpi + 48) / 96);
}

int unscale(int pixels, int dpi) {
    if (dpi <= 0) dpi = 96;
    return static_cast<int>((static_cast<std::int64_t>(pixels) * 96 + dpi / 2) / dpi);
}

Size clamp_panel_size(Size dip) {
    return {std::clamp(dip.width, kMinPanelDip.width, kMaxPanelDip.width),
            std::clamp(dip.height, kMinPanelDip.height, kMaxPanelDip.height)};
}

Result place_panel(const Input& input) {
    const Size panel = clamp_panel_size(input.panel_dip);
    Result result;

    if (input.monitors.empty()) {
        // No monitor information: fall back to a 100 % scale rectangle at the cursor.
        result.rect = {input.cursor.x, input.cursor.y, input.cursor.x + panel.width, input.cursor.y + panel.height};
        return result;
    }

    // 1. Pick the monitor.
    int chosen = 0;
    std::int64_t best = -1;
    for (int i = 0; i < static_cast<int>(input.monitors.size()); ++i) {
        const std::int64_t d = distance_squared(input.cursor, input.monitors[static_cast<std::size_t>(i)].bounds);
        if (best < 0 || d < best) {
            best = d;
            chosen = i;
        }
    }
    const Monitor& monitor = input.monitors[static_cast<std::size_t>(chosen)];
    result.monitor = chosen;
    result.dpi = monitor.dpi > 0 ? monitor.dpi : 96;

    // The usable area: the work area minus a margin, unless the work area is too small for one.
    Rect area = monitor.work;
    if (area.width() <= 0 || area.height() <= 0) area = monitor.bounds;
    const int margin = scale(input.margin_dip, result.dpi);
    if (area.width() > 4 * margin && area.height() > 4 * margin) {
        area = {area.left + margin, area.top + margin, area.right - margin, area.bottom - margin};
    }

    // 2. Size.
    const int width = std::min(scale(panel.width, result.dpi), area.width());
    const int height = std::min(scale(panel.height, result.dpi), area.height());

    // 3. Preferred corner, flipping away from an edge that is too close.
    const int gap = scale(input.gap_dip, result.dpi);
    int x = input.cursor.x + gap;
    if (x + width > area.right) x = input.cursor.x - gap - width;
    int y = input.cursor.y + gap;
    if (y + height > area.bottom) y = input.cursor.y - gap - height;

    // 4. Clamp.
    x = std::clamp(x, area.left, area.right - width);
    y = std::clamp(y, area.top, area.bottom - height);

    result.rect = {x, y, x + width, y + height};
    return result;
}

}  // namespace buddy::placement
