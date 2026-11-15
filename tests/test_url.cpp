#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/url.h"

using namespace buddy::url;

TEST_CASE("url: https endpoints are accepted and normalised", "[url]") {
    const BaseUrlResult r = parse_base_url("  HTTPS://API.Anthropic.com/v1/  ");
    REQUIRE(r.ok());
    CHECK(r.url->https);
    CHECK(r.url->host == "api.anthropic.com");
    CHECK(r.url->port == 443);
    CHECK(r.url->path == "/v1");
    CHECK(r.url->to_string() == "https://api.anthropic.com/v1");

    CHECK(parse_base_url("https://example.com").url->to_string() == "https://example.com");
    CHECK(parse_base_url("https://example.com:8443/api/v2").url->to_string() == "https://example.com:8443/api/v2");
}

TEST_CASE("url: plain http is allowed only for this PC", "[url]") {
    CHECK(parse_base_url("http://localhost:11434/v1").ok());
    CHECK(parse_base_url("http://127.0.0.1:9931/v1").url->to_string() == "http://127.0.0.1:9931/v1");
    CHECK(parse_base_url("http://127.255.0.9/v1").ok());
    CHECK(parse_base_url("http://[::1]:1234/v1").url->port == 1234);
    CHECK(parse_base_url("http://localhost").url->port == 80);

    for (const char* remote : {"http://example.com/v1", "http://192.168.1.10:8080/v1", "http://10.0.0.5/v1",
                               "http://127.0.0.1.example.com/v1", "http://localhost.example.com/v1",
                               "http://128.0.0.1/v1", "http://0.0.0.0:8080/v1"}) {
        INFO(remote);
        CHECK_FALSE(parse_base_url(remote).ok());
    }
}

TEST_CASE("url: loopback detection", "[url]") {
    CHECK(is_loopback_host("localhost"));
    CHECK(is_loopback_host("LOCALHOST"));
    CHECK(is_loopback_host("127.0.0.1"));
    CHECK(is_loopback_host("127.1.2.3"));
    CHECK(is_loopback_host("[::1]"));
    CHECK_FALSE(is_loopback_host("127.0.0.1.evil.example"));
    CHECK_FALSE(is_loopback_host("127.0.0.256"));
    CHECK_FALSE(is_loopback_host("127.0.0"));
    CHECK_FALSE(is_loopback_host("192.168.0.1"));
    CHECK_FALSE(is_loopback_host("example.com"));
    CHECK_FALSE(is_loopback_host(""));
}

TEST_CASE("url: malformed and dangerous endpoints are refused with a message", "[url]") {
    for (const char* bad : {"", "   ", "api.anthropic.com/v1", "ftp://example.com", "file:///C:/x", "javascript:alert(1)",
                            "https://", "https:///v1", "https://user:pass@example.com/v1", "https://example.com/v1?key=1",
                            "https://example.com/v1#frag", "https://example.com:0/v1", "https://example.com:99999/v1",
                            "https://example.com:abc/v1", "https://example.com:/v1", "https://exa mple.com/v1",
                            "https://example.com/a b", "https://example.com/../x", "https://example.com//x",
                            "https://example.com/v1\r\nHost: evil", "https://[::1/v1"}) {
        INFO("url: " << bad);
        const BaseUrlResult r = parse_base_url(bad);
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.error.empty());
    }
    CHECK_FALSE(parse_base_url("https://example.com/" + std::string(3000, 'a')).ok());
}
