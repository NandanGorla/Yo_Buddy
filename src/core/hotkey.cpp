#include "core/hotkey.h"

#include <array>
#include <vector>

#include "core/text.h"

namespace buddy::hotkey {
namespace {

struct NamedKey {
    std::string_view name;      // canonical spelling, used by format()
    std::string_view lower;     // the same in lower case, used by parse()
    unsigned key;               // virtual-key code
};

// Virtual-key codes are written out as numbers so this file needs no Windows header.
constexpr std::array<NamedKey, 14> kNamedKeys = {{
    {"Space", "space", 0x20},
    {"Tab", "tab", 0x09},
    {"Enter", "enter", 0x0D},
    {"Backspace", "backspace", 0x08},
    {"Insert", "insert", 0x2D},
    {"Delete", "delete", 0x2E},
    {"Home", "home", 0x24},
    {"End", "end", 0x23},
    {"PageUp", "pageup", 0x21},
    {"PageDown", "pagedown", 0x22},
    {"Left", "left", 0x25},
    {"Up", "up", 0x26},
    {"Right", "right", 0x27},
    {"Down", "down", 0x28},
}};

constexpr unsigned kVkF1 = 0x70;
constexpr unsigned kVkF4 = 0x73;
constexpr unsigned kVkF12 = 0x7B;
constexpr unsigned kVkTab = 0x09;
constexpr unsigned kVkSpace = 0x20;
constexpr unsigned kVkDelete = 0x2E;

ParseResult fail(std::string message) {
    ParseResult r;
    r.error = std::move(message);
    return r;
}

// Returns the virtual-key code for a key name (already lower case), or 0.
unsigned key_from_name(std::string_view name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c);
        return 0;
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
        int number = 0;
        for (const char c : name.substr(1)) {
            if (c < '0' || c > '9') return 0;
            number = number * 10 + (c - '0');
        }
        if (number >= 1 && number <= 24) return kVkF1 + static_cast<unsigned>(number - 1);
        return 0;
    }
    for (const NamedKey& k : kNamedKeys) {
        if (k.lower == name) return k.key;
    }
    return 0;
}

std::string key_name(unsigned key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    if (key >= kVkF1 && key < kVkF1 + 24) return "F" + std::to_string(key - kVkF1 + 1);
    for (const NamedKey& k : kNamedKeys) {
        if (k.key == key) return std::string(k.name);
    }
    return "?";
}

}  // namespace

ParseResult parse(std::string_view text) {
    text = text::trim_ascii(text);
    if (text.empty()) return fail("Type a shortcut such as Alt+Shift+Space.");
    if (text.size() > 64) return fail("That shortcut is too long.");

    Binding binding;
    bool have_key = false;
    std::string_view rest = text;
    while (true) {
        const auto plus = rest.find('+');
        const std::string part = text::to_lower_ascii(text::trim_ascii(rest.substr(0, plus)));
        if (part.empty()) return fail("That shortcut has an empty part. Write it like Ctrl+Alt+B.");

        if (part == "ctrl" || part == "control") {
            binding.modifiers |= kCtrl;
        } else if (part == "alt") {
            binding.modifiers |= kAlt;
        } else if (part == "shift") {
            binding.modifiers |= kShift;
        } else if (part == "win" || part == "windows" || part == "meta" || part == "super") {
            return fail("Shortcuts with the Windows key are reserved for Windows.");
        } else {
            if (have_key) return fail("A shortcut can have only one key besides Ctrl, Alt and Shift.");
            const unsigned key = key_from_name(part);
            if (key == 0) return fail("\"" + part + "\" is not a key Buddy can use in a shortcut.");
            binding.key = key;
            have_key = true;
        }
        if (plus == std::string_view::npos) break;
        rest.remove_prefix(plus + 1);
    }

    if (!have_key) return fail("Add a key to the shortcut, for example Alt+Shift+Space.");
    if ((binding.modifiers & (kCtrl | kAlt)) == 0) {
        return fail("The shortcut must include Ctrl or Alt, or it would get in the way of typing.");
    }
    if (binding.key == kVkF12) return fail("F12 is reserved by Windows for debuggers.");

    // Combinations that belong to Windows itself.
    const bool alt_only = binding.modifiers == kAlt;
    if (alt_only && (binding.key == kVkF4 || binding.key == kVkTab || binding.key == kVkSpace)) {
        return fail("Windows already uses that shortcut.");
    }
    if (binding.modifiers == (kCtrl | kAlt) && binding.key == kVkDelete) {
        return fail("Windows already uses that shortcut.");
    }
    return ParseResult{binding, {}};
}

std::string format(const Binding& binding) {
    std::string out;
    if (binding.modifiers & kCtrl) out += "Ctrl+";
    if (binding.modifiers & kAlt) out += "Alt+";
    if (binding.modifiers & kShift) out += "Shift+";
    out += key_name(binding.key);
    return out;
}

const char* to_code(Status status) {
    switch (status) {
        case Status::None: return "none";
        case Status::Active: return "active";
        case Status::Taken: return "taken";
        case Status::Invalid: return "invalid";
        case Status::Failed: return "failed";
    }
    return "none";
}

Manager::Manager(IRegistrar& registrar) : registrar_(registrar) {}

Manager::~Manager() { release(); }

void Manager::release() {
    if (active_) registrar_.unregister_hotkey(active_id_);
    active_.reset();
    active_id_ = 0;
}

std::string Manager::active() const { return active_ ? format(*active_) : std::string(); }

Status Manager::apply(std::string_view text) {
    requested_ = std::string(text::trim_ascii(text));
    message_.clear();

    const ParseResult parsed = parse(requested_);
    if (!parsed.ok()) {
        message_ = parsed.error;
        return status_ = Status::Invalid;
    }
    requested_ = format(*parsed.binding);   // show the canonical spelling from here on

    if (active_ && *active_ == *parsed.binding) return status_ = Status::Active;   // nothing to change

    // Register the new binding under the id that is not in use, and only then let go of the old
    // one. If the new binding is unavailable the old hotkey keeps working.
    const int new_id = active_id_ == 1 ? 2 : 1;
    switch (registrar_.register_hotkey(new_id, *parsed.binding)) {
        case RegisterOutcome::Ok: break;
        case RegisterOutcome::AlreadyTaken:
            message_ = requested_ + " is already used by another app.";
            if (active_) message_ += " Buddy keeps using " + format(*active_) + ".";
            return status_ = Status::Taken;
        case RegisterOutcome::Failed:
            message_ = "Windows could not register " + requested_ + ".";
            return status_ = Status::Failed;
    }
    if (active_) registrar_.unregister_hotkey(active_id_);
    active_ = parsed.binding;
    active_id_ = new_id;
    return status_ = Status::Active;
}

}  // namespace buddy::hotkey
