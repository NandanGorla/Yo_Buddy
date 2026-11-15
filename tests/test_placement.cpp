// Window placement is a pure function: cursor + monitors + work areas + panel size -> rectangle.
// These tests describe desktops with plain numbers, so layouts the test machine does not have
// (several monitors, mixed DPI, a monitor to the left of the primary) are covered too.
#include <catch2/catch_test_macros.hpp>

#include "core/placement.h"

using namespace buddy::placement;

namespace {

// A 1920x1080 monitor at 100 % with a 48 px taskbar at the bottom.
Monitor full_hd(int left = 0, int top = 0) {
    return {{left, top, left + 1920, top + 1080}, {left, top, left + 1920, top + 1032}, 96};
}

Input input_for(Point cursor, std::vector<Monitor> monitors, Size panel = {440, 560}) {
    Input in;
    in.cursor = cursor;
    in.monitors = std::move(monitors);
    in.panel_dip = panel;
    return in;
}

bool inside(const Rect& inner, const Rect& outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

}  // namespace

TEST_CASE("placement: with room to spare the panel sits below and right of the cursor", "[placement]") {
    const Result r = place_panel(input_for({500, 300}, {full_hd()}));
    CHECK(r.monitor == 0);
    CHECK(r.dpi == 96);
    CHECK(r.rect == Rect{514, 314, 954, 874});   // cursor + 14 px gap, 440 x 560
    CHECK_FALSE(r.rect.contains({500, 300}));
}

TEST_CASE("placement: near the right edge the panel flips to the left of the cursor", "[placement]") {
    const Result r = place_panel(input_for({1800, 300}, {full_hd()}));
    CHECK(r.rect.right == 1800 - 14);
    CHECK(r.rect.width() == 440);
    CHECK(r.rect.top == 314);
    CHECK_FALSE(r.rect.contains({1800, 300}));
}

TEST_CASE("placement: near the bottom edge the panel flips above the cursor", "[placement]") {
    const Result r = place_panel(input_for({500, 1000}, {full_hd()}));
    CHECK(r.rect.bottom == 1000 - 14);
    CHECK(r.rect.left == 514);
    CHECK_FALSE(r.rect.contains({500, 1000}));
}

TEST_CASE("placement: in the bottom-right corner it flips both ways", "[placement]") {
    const Result r = place_panel(input_for({1900, 1020}, {full_hd()}));
    CHECK(r.rect.right == 1900 - 14);
    CHECK(r.rect.bottom == 1020 - 14);
}

TEST_CASE("placement: the panel always stays inside the work area", "[placement]") {
    const Monitor m = full_hd();
    // Sweep the cursor over the whole monitor, including the taskbar strip.
    for (int x = 0; x < 1920; x += 37) {
        for (int y = 0; y < 1080; y += 29) {
            const Result r = place_panel(input_for({x, y}, {m}));
            INFO("cursor " << x << "," << y);
            REQUIRE(inside(r.rect, m.work));
            REQUIRE(r.rect.width() == 440);
            REQUIRE(r.rect.height() == 560);
        }
    }
}

TEST_CASE("placement: a margin is kept from the work-area edges", "[placement]") {
    // The cursor is in the taskbar, so the panel is pushed up against the bottom of the work area.
    const Result r = place_panel(input_for({5, 1075}, {full_hd()}));
    CHECK(r.rect.bottom <= 1032 - 8);
    CHECK(r.rect.left >= 8);
}

TEST_CASE("placement: the size scales with the monitor's DPI", "[placement]") {
    // A 3840x2160 monitor at 150 %.
    const Monitor hidpi{{0, 0, 3840, 2160}, {0, 0, 3840, 2088}, 144};
    const Result r = place_panel(input_for({1000, 500}, {hidpi}));
    CHECK(r.dpi == 144);
    CHECK(r.rect.width() == 660);    // 440 * 1.5
    CHECK(r.rect.height() == 840);   // 560 * 1.5
    CHECK(r.rect.left == 1000 + 21); // the gap scales too: 14 * 1.5
    CHECK(r.rect.top == 500 + 21);
}

TEST_CASE("placement: the panel opens on the monitor under the cursor", "[placement]") {
    // Primary 1920x1080 at 100 %, a 2560x1440 monitor at 125 % to its right, and a portrait
    // monitor at 100 % to its left (negative coordinates).
    const Monitor primary = full_hd();
    const Monitor right{{1920, 0, 4480, 1440}, {1920, 0, 4480, 1392}, 120};
    const Monitor left{{-1080, 0, 0, 1920}, {-1080, 0, 0, 1872}, 96};
    const std::vector<Monitor> desktop = {primary, right, left};

    const Result on_right = place_panel(input_for({3000, 700}, desktop));
    CHECK(on_right.monitor == 1);
    CHECK(on_right.dpi == 120);
    CHECK(on_right.rect.width() == 550);   // 440 * 1.25
    CHECK(inside(on_right.rect, right.work));

    const Result on_left = place_panel(input_for({-500, 900}, desktop));
    CHECK(on_left.monitor == 2);
    CHECK(on_left.rect.width() == 440);
    CHECK(inside(on_left.rect, left.work));

    const Result on_primary = place_panel(input_for({100, 100}, desktop));
    CHECK(on_primary.monitor == 0);
    CHECK(inside(on_primary.rect, primary.work));
}

TEST_CASE("placement: the panel never straddles two monitors", "[placement]") {
    const Monitor primary = full_hd();
    const Monitor right{{1920, 0, 4480, 1440}, {1920, 0, 4480, 1392}, 120};
    // Cursor at the last column of the primary: the panel must stay on the primary.
    const Result r = place_panel(input_for({1919, 400}, {primary, right}));
    CHECK(r.monitor == 0);
    CHECK(inside(r.rect, primary.work));
    // One pixel further it belongs to the right monitor.
    const Result r2 = place_panel(input_for({1920, 400}, {primary, right}));
    CHECK(r2.monitor == 1);
    CHECK(inside(r2.rect, right.work));
}

TEST_CASE("placement: a cursor outside every monitor uses the nearest one", "[placement]") {
    const Monitor primary = full_hd();
    const Monitor right{{1920, 0, 4480, 1440}, {1920, 0, 4480, 1392}, 120};
    const Result r = place_panel(input_for({6000, 5000}, {primary, right}));
    CHECK(r.monitor == 1);
    CHECK(inside(r.rect, right.work));
}

TEST_CASE("placement: a taskbar on the left or top is respected", "[placement]") {
    const Monitor left_bar{{0, 0, 1920, 1080}, {64, 0, 1920, 1080}, 96};
    CHECK(place_panel(input_for({10, 500}, {left_bar})).rect.left >= 64);

    const Monitor top_bar{{0, 0, 1920, 1080}, {0, 48, 1920, 1080}, 96};
    CHECK(place_panel(input_for({500, 5}, {top_bar})).rect.top >= 48);
}

TEST_CASE("placement: a panel larger than the work area is shrunk to fit", "[placement]") {
    const Monitor small{{0, 0, 800, 600}, {0, 0, 800, 560}, 96};
    const Result r = place_panel(input_for({400, 300}, {small}, {1600, 1400}));
    CHECK(inside(r.rect, small.work));
    CHECK(r.rect.width() <= 800);
    CHECK(r.rect.height() <= 560);
}

TEST_CASE("placement: a tiny work area still yields a rectangle inside it", "[placement]") {
    const Monitor tiny{{0, 0, 200, 150}, {0, 0, 200, 150}, 96};
    const Result r = place_panel(input_for({100, 75}, {tiny}));
    CHECK(inside(r.rect, tiny.work));
    CHECK(r.rect.width() > 0);
    CHECK(r.rect.height() > 0);
}

TEST_CASE("placement: stored sizes are clamped to the allowed range", "[placement]") {
    CHECK(clamp_panel_size({10, 10}).width == kMinPanelDip.width);
    CHECK(clamp_panel_size({10, 10}).height == kMinPanelDip.height);
    CHECK(clamp_panel_size({99999, 99999}).width == kMaxPanelDip.width);
    CHECK(clamp_panel_size({500, 600}).width == 500);
    // place_panel applies the same clamp.
    CHECK(place_panel(input_for({500, 300}, {full_hd()}, {1, 1})).rect.width() == kMinPanelDip.width);
}

TEST_CASE("placement: without monitor information it falls back to the cursor position", "[placement]") {
    const Result r = place_panel(input_for({300, 200}, {}));
    CHECK(r.monitor == -1);
    CHECK(r.rect == Rect{300, 200, 740, 760});
}

TEST_CASE("placement: a monitor reporting a zero DPI is treated as 100 %", "[placement]") {
    Monitor odd = full_hd();
    odd.dpi = 0;
    const Result r = place_panel(input_for({500, 300}, {odd}));
    CHECK(r.dpi == 96);
    CHECK(r.rect.width() == 440);
}

TEST_CASE("placement: DIP conversion rounds and round-trips", "[placement]") {
    CHECK(scale(440, 96) == 440);
    CHECK(scale(440, 120) == 550);
    CHECK(scale(440, 144) == 660);
    CHECK(scale(7, 120) == 9);       // 8.75 rounds up
    CHECK(unscale(550, 120) == 440);
    CHECK(unscale(660, 144) == 440);
    for (const int dpi : {96, 120, 144, 168, 192, 240}) {
        CHECK(unscale(scale(440, dpi), dpi) == 440);
        CHECK(unscale(scale(563, dpi), dpi) == 563);
    }
}
