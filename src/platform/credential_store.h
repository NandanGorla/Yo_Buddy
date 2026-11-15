#pragma once

#include <string>

#include "core/credentials.h"

namespace buddy {

// ICredentialStore backed by Windows Credential Manager (generic credentials, encrypted by
// Windows for the signed-in user). Entries show up in Control Panel > Credential Manager as
// "<prefix><name>", e.g. "Buddy/apikey/anthropic".
class WinCredentialStore final : public ICredentialStore {
public:
    explicit WinCredentialStore(std::wstring target_prefix = L"Buddy/");

    bool set(std::string_view name, std::string_view secret) override;
    std::optional<std::string> get(std::string_view name) override;
    bool erase(std::string_view name) override;

private:
    std::wstring target_for(std::string_view name) const;

    std::wstring prefix_;
};

}  // namespace buddy
