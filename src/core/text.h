#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace buddy::text {

// Cuts `s` to at most `max_bytes` without splitting a UTF-8 code point.
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);

// ASCII-only helpers. They never consult the C locale.
bool is_ascii_alnum(unsigned char c);
std::string to_lower_ascii(std::string_view s);
std::string_view trim_ascii(std::string_view s);

// True when every byte is printable ASCII (0x21..0x7E), i.e. no spaces and no control bytes.
// Used for anything that ends up in an HTTP header, so CR/LF can never be injected.
bool is_printable_ascii_token(std::string_view s);

}  // namespace buddy::text
