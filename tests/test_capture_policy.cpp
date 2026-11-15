#include <catch2/catch_test_macros.hpp>

#include "core/capture_policy.h"

using namespace buddy::capture;

TEST_CASE("capture policy: exclusion matching covers process and title", "[capture]") {
    const std::vector<std::string> exclusions = {"1Password", "Windows Security", "private"};
    CHECK(is_excluded({"1Password.exe", "Vault"}, exclusions));
    CHECK(is_excluded({"explorer.exe", "Windows Security - Credentials"}, exclusions));
    CHECK(is_excluded({"browser.exe", "Private window"}, exclusions));
    CHECK_FALSE(is_excluded({"notepad.exe", "Notes"}, exclusions));
    CHECK_FALSE(is_excluded({"browser.exe", "Public window"}, exclusions));
}

TEST_CASE("capture policy: remote sends require preview", "[capture]") {
    CHECK_FALSE(requires_preview(true, true));
    CHECK_FALSE(requires_preview(true, false));
    CHECK(requires_preview(false, true));
    CHECK_FALSE(requires_preview(false, false));
}
