#include "platform/credential_store.h"

#include <windows.h>
#include <wincred.h>

#include "platform/win_text.h"

namespace buddy {

WinCredentialStore::WinCredentialStore(std::wstring target_prefix) : prefix_(std::move(target_prefix)) {}

std::wstring WinCredentialStore::target_for(std::string_view name) const { return prefix_ + win::widen(name); }

bool WinCredentialStore::set(std::string_view name, std::string_view secret) {
    if (secret.empty() || secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;

    std::wstring target = target_for(name);
    std::wstring user = L"Buddy";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.UserName = user.data();
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    // LOCAL_MACHINE persistence keeps the entry on this PC for this user; it does not roam.
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

std::optional<std::string> WinCredentialStore::get(std::string_view name) {
    const std::wstring target = target_for(name);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return std::nullopt;

    std::optional<std::string> secret;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        secret.emplace(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        // Wipe Windows' copy of the secret before releasing the buffer.
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    }
    CredFree(credential);
    return secret;
}

bool WinCredentialStore::erase(std::string_view name) {
    const std::wstring target = target_for(name);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;   // already absent counts as erased
}

}  // namespace buddy
