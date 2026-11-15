#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace buddy::capture {

struct WindowIdentity {
    std::string process_name;
    std::string title;
};

bool is_excluded(const WindowIdentity& window, const std::vector<std::string>& exclusions);
bool requires_preview(bool endpoint_is_loopback, bool preview_setting);

}  // namespace buddy::capture
