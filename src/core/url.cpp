#include "core/url.h"

#include <vector>

#include "core/text.h"

namespace buddy::url {
namespace {

BaseUrlResult fail(std::string message) {
    BaseUrlResult r;
    r.error = std::move(message);
    return r;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && text::to_lower_ascii(s.substr(0, prefix.size())) == prefix;
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool is_host_char(unsigned char c) { return text::is_ascii_alnum(c) || c == '.' || c == '-'; }

bool is_path_char(unsigned char c) {
    return text::is_ascii_alnum(c) || c == '/' || c == '.' || c == '_' || c == '~' || c == '-';
}

}  // namespace

std::string BaseUrl::to_string() const {
    std::string out = https ? "https://" : "http://";
    out += host;
    const int default_port = https ? 443 : 80;
    if (port != default_port) out += ":" + std::to_string(port);
    out += path;
    return out;
}

bool is_loopback_host(std::string_view host) {
    const std::string h = text::to_lower_ascii(host);
    if (h == "localhost" || h == "[::1]") return true;

    // 127.0.0.0/8 written as a dotted quad.
    std::vector<std::string_view> parts;
    std::string_view rest = h;
    while (true) {
        const auto dot = rest.find('.');
        parts.push_back(rest.substr(0, dot));
        if (dot == std::string_view::npos) break;
        rest.remove_prefix(dot + 1);
    }
    if (parts.size() != 4 || parts[0] != "127") return false;
    for (const auto part : parts) {
        if (!all_digits(part) || part.size() > 3 || std::stoi(std::string(part)) > 255) return false;
    }
    return true;
}

BaseUrlResult parse_base_url(std::string_view input) {
    std::string_view s = text::trim_ascii(input);
    if (s.empty()) return fail("The endpoint URL is empty.");
    if (s.size() > 2048) return fail("The endpoint URL is too long.");

    BaseUrl out;
    if (starts_with_ci(s, "https://")) {
        out.https = true;
        s.remove_prefix(8);
    } else if (starts_with_ci(s, "http://")) {
        out.https = false;
        s.remove_prefix(7);
    } else {
        return fail("The endpoint URL must start with https:// (or http:// for a server on this PC).");
    }

    if (s.find_first_of("?#") != std::string_view::npos) {
        return fail("The endpoint URL must not contain a query string or fragment.");
    }

    const auto slash = s.find('/');
    std::string_view authority = s.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? std::string_view{} : s.substr(slash);

    if (authority.find('@') != std::string_view::npos) {
        return fail("The endpoint URL must not contain a user name or password.");
    }

    // Split host and port. An IPv6 literal is bracketed, so its colons are not the port separator.
    std::string_view host = authority;
    std::string_view port_text;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return fail("The endpoint URL has an unterminated IPv6 address.");
        host = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return fail("The endpoint URL has unexpected text after the address.");
            port_text = after.substr(1);
            if (port_text.empty()) return fail("The endpoint URL has an empty port.");
        }
        for (const char c : host.substr(1, host.size() - 2)) {
            const auto u = static_cast<unsigned char>(c);
            if (!text::is_ascii_alnum(u) && c != ':' && c != '.') {
                return fail("The endpoint URL has an invalid IPv6 address.");
            }
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) return fail("The endpoint URL has an empty port.");
        }
        if (host.empty()) return fail("The endpoint URL has no host name.");
        for (const char c : host) {
            if (!is_host_char(static_cast<unsigned char>(c))) {
                return fail("The endpoint URL has an invalid host name.");
            }
        }
    }
    if (host.size() > 253) return fail("The endpoint URL host name is too long.");

    out.host = text::to_lower_ascii(host);
    out.port = out.https ? 443 : 80;
    if (!port_text.empty()) {
        if (!all_digits(port_text) || port_text.size() > 5) return fail("The endpoint URL has an invalid port.");
        const int port = std::stoi(std::string(port_text));
        if (port < 1 || port > 65535) return fail("The endpoint URL has an invalid port.");
        out.port = port;
    }

    for (const char c : path) {
        if (!is_path_char(static_cast<unsigned char>(c))) {
            return fail("The endpoint URL path has unsupported characters.");
        }
    }
    if (path.find("//") != std::string_view::npos || path.find("..") != std::string_view::npos) {
        return fail("The endpoint URL path is not valid.");
    }
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    out.path = std::string(path);

    if (!out.https && !is_loopback_host(out.host)) {
        return fail("Plain http is only allowed for a server on this PC. Use https for remote endpoints.");
    }
    return BaseUrlResult{out, {}};
}

}  // namespace buddy::url
