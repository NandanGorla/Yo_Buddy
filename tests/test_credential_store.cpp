#include <catch2/catch_test_macros.hpp>

#include <string>

#include "platform/credential_store.h"

using buddy::WinCredentialStore;

// These tests use the real Windows Credential Manager of the signed-in user, under a prefix that
// only tests use ("BuddyTest/"), and remove every entry they create.
TEST_CASE("credential store: set, get, overwrite and erase", "[credentials]") {
    WinCredentialStore store(L"BuddyTest/");
    const std::string name = "apikey/unit-test";
    store.erase(name);

    CHECK_FALSE(store.get(name).has_value());

    REQUIRE(store.set(name, "sk-first-secret"));
    CHECK(store.get(name) == std::optional<std::string>("sk-first-secret"));

    REQUIRE(store.set(name, "sk-second-secret"));
    CHECK(store.get(name) == std::optional<std::string>("sk-second-secret"));

    CHECK(store.erase(name));
    CHECK_FALSE(store.get(name).has_value());
    CHECK(store.erase(name));   // erasing something already gone is still a success
}

TEST_CASE("credential store: entries are separate per name", "[credentials]") {
    WinCredentialStore store(L"BuddyTest/");
    REQUIRE(store.set("apikey/a", "secret-a"));
    REQUIRE(store.set("apikey/b", "secret-b"));
    CHECK(store.get("apikey/a") == std::optional<std::string>("secret-a"));
    CHECK(store.get("apikey/b") == std::optional<std::string>("secret-b"));
    CHECK(store.erase("apikey/a"));
    CHECK(store.get("apikey/b").has_value());
    CHECK(store.erase("apikey/b"));
}

TEST_CASE("credential store: empty and oversized secrets are refused", "[credentials]") {
    WinCredentialStore store(L"BuddyTest/");
    CHECK_FALSE(store.set("apikey/empty", ""));
    CHECK_FALSE(store.set("apikey/huge", std::string(3000, 'x')));   // Windows caps a secret at 2560 bytes
    CHECK_FALSE(store.get("apikey/empty").has_value());
    CHECK_FALSE(store.get("apikey/huge").has_value());
}
