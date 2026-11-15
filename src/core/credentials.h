#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace buddy {

// Where secrets (API keys) are kept. The real implementation is Windows Credential Manager
// (platform/credential_store.h); tests use an in-memory fake.
class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;

    virtual bool set(std::string_view name, std::string_view secret) = 0;
    virtual std::optional<std::string> get(std::string_view name) = 0;
    virtual bool erase(std::string_view name) = 0;   // true when the entry is gone afterwards
};

}  // namespace buddy
