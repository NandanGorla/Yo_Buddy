#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace buddy::url {

struct BaseUrl {
    bool https = true;
    std::string host;   // lower case; an IPv6 literal keeps its brackets
    int port = 443;
    std::string path;   // "" or "/v1"; never ends in '/'

    // "https://api.example.com/v1". The port is written only when it is not the scheme default.
    std::string to_string() const;
};

struct BaseUrlResult {
    std::optional<BaseUrl> url;
    std::string error;   // set when `url` is empty; written for the user

    bool ok() const { return url.has_value(); }
};

// Validates the endpoint a provider talks to.
//  - Scheme must be https. Plain http is accepted only for loopback hosts, because anything else
//    would send the API key and the conversation in clear text.
//  - No user info, query string or fragment.
BaseUrlResult parse_base_url(std::string_view input);

// "localhost", 127.0.0.0/8 or [::1]. PROJECT.md: "local" means loopback only.
bool is_loopback_host(std::string_view host);

}  // namespace buddy::url
