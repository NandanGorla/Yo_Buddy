#pragma once

#include <optional>
#include <string>
#include <string_view>

// The global hotkey that summons the floating panel: parsing and formatting of bindings such as
// "Alt+Shift+Space", and the logic for switching from one binding to another when the new one
// may already belong to a different application.
//
// Nothing here calls Windows. The real RegisterHotKey call sits behind IRegistrar, so the
// conflict handling can be unit-tested with a fake.
namespace buddy::hotkey {

// Same bit values as the Win32 MOD_* flags, so they can be passed straight to RegisterHotKey.
inline constexpr unsigned kAlt = 0x1;
inline constexpr unsigned kCtrl = 0x2;
inline constexpr unsigned kShift = 0x4;

struct Binding {
    unsigned modifiers = 0;   // a combination of kAlt, kCtrl, kShift
    unsigned key = 0;         // a Windows virtual-key code

    bool operator==(const Binding&) const = default;
};

// Chosen because nothing in the default keymaps of Rider, PyCharm or CLion uses it (checked on
// 2026-11-15), and Windows itself does not. Alt+Space is the window menu, Ctrl+Space and
// Ctrl+Shift+Space are code completion, Ctrl+Alt+Space is class-name completion.
inline constexpr const char* kDefault = "Alt+Shift+Space";

struct ParseResult {
    std::optional<Binding> binding;
    std::string error;   // written for the user

    bool ok() const { return binding.has_value(); }
};

// Accepts modifiers (Ctrl, Alt, Shift) joined to one key with '+', in any order and letter case.
// Keys: A-Z, 0-9, F1-F24 except F12, Space, Tab, Enter, Backspace, Insert, Delete, Home, End,
// PageUp, PageDown and the arrow keys.
// Refused: bindings without Ctrl or Alt (they would swallow ordinary typing), the Windows key
// (reserved for the system), F12 (reserved for debuggers) and combinations Windows owns.
ParseResult parse(std::string_view text);

// Canonical text, modifiers in the order Ctrl, Alt, Shift: "Ctrl+Alt+B".
std::string format(const Binding& binding);

enum class RegisterOutcome { Ok, AlreadyTaken, Failed };

class IRegistrar {
public:
    virtual ~IRegistrar() = default;
    virtual RegisterOutcome register_hotkey(int id, const Binding& binding) = 0;
    virtual void unregister_hotkey(int id) = 0;
};

enum class Status {
    None,      // nothing has been requested yet
    Active,    // the requested binding is registered
    Taken,     // another application already owns the requested binding
    Invalid,   // the requested text is not an acceptable binding
    Failed,    // Windows refused for another reason
};
const char* to_code(Status status);

// Owns "which binding is live". The important property: a failed change never costs the user
// their working hotkey. The new binding is registered first, and the old one is released only
// after that succeeded.
class Manager {
public:
    explicit Manager(IRegistrar& registrar);
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Tries to make `text` the live binding and returns the outcome for that request.
    Status apply(std::string_view text);

    void release();   // unregisters whatever is live

    Status status() const { return status_; }                 // outcome of the latest request
    const std::string& requested() const { return requested_; }   // text of the latest request
    const std::string& message() const { return message_; }   // user-facing detail when not Active
    std::string active() const;                               // the live binding, or "" when none
    std::optional<Binding> active_binding() const { return active_; }
    int active_id() const { return active_id_; }              // id carried by WM_HOTKEY, 0 when none

private:
    IRegistrar& registrar_;
    std::optional<Binding> active_;
    int active_id_ = 0;
    Status status_ = Status::None;
    std::string requested_;
    std::string message_;
};

}  // namespace buddy::hotkey
