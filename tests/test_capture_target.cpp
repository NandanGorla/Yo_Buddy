#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "platform/capture_target.h"

using namespace buddy::capture;

TEST_CASE("capture target: invalid window is rejected", "[capture]") {
    const WindowIdentityResult result = identify_window(nullptr);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("capture target: invalid handles are never treated as Buddy windows", "[capture]") {
    CHECK_FALSE(is_buddy_window(nullptr));
}

TEST_CASE("capture target: current process window identity is bounded", "[capture]") {
    HWND console = GetConsoleWindow();
    if (!console) {
        SUCCEED("No console window is available in this test host.");
        return;
    }

    const WindowIdentityResult result = identify_window(console);
    REQUIRE(result.ok());
    CHECK_FALSE(result.identity->process_name.empty());
    CHECK(result.identity->process_name.size() <= 32768);
    CHECK(result.identity->title.size() <= 512);
}

TEST_CASE("capture target: foreground target resolution returns a usable target", "[capture]") {
    const TargetResult window = resolve_foreground_target(false);
    const TargetResult monitor = resolve_foreground_target(true);
    if (!window.ok() || !monitor.ok()) {
        SUCCEED("The test host has no foreground window or monitor.");
        return;
    }
    CHECK(window.window != nullptr);
    REQUIRE(monitor.monitor.has_value());
    CHECK(monitor.monitor->bounds.right > monitor.monitor->bounds.left);
    CHECK(monitor.monitor->bounds.bottom > monitor.monitor->bounds.top);
}
