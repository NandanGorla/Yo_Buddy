#include "core/capture_policy.h"

#include <algorithm>
#include <cctype>

namespace buddy::capture {
namespace {

std::string lower_ascii(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (const unsigned char c : input) result.push_back(static_cast<char>(std::tolower(c)));
    return result;
}

bool contains_case_insensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) return false;
    const std::string lower_value = lower_ascii(value);
    const std::string lower_needle = lower_ascii(needle);
    return lower_value.find(lower_needle) != std::string::npos;
}

}  // namespace

bool is_excluded(const WindowIdentity& window, const std::vector<std::string>& exclusions) {
    for (const std::string& exclusion : exclusions) {
        if (contains_case_insensitive(window.process_name, exclusion) ||
            contains_case_insensitive(window.title, exclusion)) {
            return true;
        }
    }
    return false;
}

bool requires_preview(bool endpoint_is_loopback, bool preview_setting) {
    return !endpoint_is_loopback && preview_setting;
}

}  // namespace buddy::capture
