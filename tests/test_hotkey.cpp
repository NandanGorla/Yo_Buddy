// Hotkey parsing, and what happens when the wanted hotkey already belongs to another app.
// The real RegisterHotKey call is replaced by a fake that plays "the rest of the system".
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "core/hotkey.h"

using namespace buddy::hotkey;

namespace {

Binding must_parse(const std::string& text) {
    const ParseResult r = parse(text);
    INFO(text << " -> " << r.error);
    REQUIRE(r.ok());
    return *r.binding;
}

// Pretends to be Windows. `taken_elsewhere` are bindings other applications hold.
class FakeRegistrar final : public IRegistrar {
public:
    std::vector<Binding> taken_elsewhere;
    bool fail_everything = false;
    std::map<int, Binding> ours;        // what Buddy currently has registered, by id
    std::vector<std::string> calls;     // the order of operations, for the tests that care

    RegisterOutcome register_hotkey(int id, const Binding& binding) override {
        calls.push_back("register " + format(binding));
        if (fail_everything) return RegisterOutcome::Failed;
        for (const Binding& other : taken_elsewhere) {
            if (other == binding) return RegisterOutcome::AlreadyTaken;
        }
        // Windows also refuses a combination this same thread already registered under another id.
        for (const auto& [existing_id, existing] : ours) {
            if (existing == binding && existing_id != id) return RegisterOutcome::AlreadyTaken;
        }
        ours[id] = binding;
        return RegisterOutcome::Ok;
    }
    void unregister_hotkey(int id) override {
        calls.push_back("unregister " + std::to_string(id));
        ours.erase(id);
    }
};

}  // namespace

TEST_CASE("hotkey: parsing accepts the usual spellings", "[hotkey]") {
    CHECK(must_parse("Alt+Shift+Space") == Binding{kAlt | kShift, 0x20});
    CHECK(must_parse("alt + shift + space") == Binding{kAlt | kShift, 0x20});
    CHECK(must_parse("Space+Shift+Alt") == Binding{kAlt | kShift, 0x20});   // any order
    CHECK(must_parse("Ctrl+Alt+B") == Binding{kCtrl | kAlt, 'B'});
    CHECK(must_parse("control+alt+b") == Binding{kCtrl | kAlt, 'B'});
    CHECK(must_parse("Ctrl+Shift+7") == Binding{kCtrl | kShift, '7'});
    CHECK(must_parse("Ctrl+F1") == Binding{kCtrl, 0x70});
    CHECK(must_parse("Alt+F24") == Binding{kAlt, 0x87});
    CHECK(must_parse("Ctrl+Alt+PageDown") == Binding{kCtrl | kAlt, 0x22});
    CHECK(must_parse("  Ctrl+Alt+Up  ") == Binding{kCtrl | kAlt, 0x26});
}

TEST_CASE("hotkey: the default binding is valid", "[hotkey]") {
    CHECK(must_parse(kDefault) == Binding{kAlt | kShift, 0x20});
    CHECK(format(must_parse(kDefault)) == kDefault);
}

TEST_CASE("hotkey: formatting is canonical and round-trips", "[hotkey]") {
    CHECK(format(must_parse("shift+alt+space")) == "Alt+Shift+Space");
    CHECK(format(must_parse("shift+ctrl+alt+k")) == "Ctrl+Alt+Shift+K");
    for (const char* text : {"Ctrl+Alt+B", "Alt+Shift+Space", "Ctrl+F5", "Ctrl+Alt+Shift+F24", "Alt+9",
                             "Ctrl+Alt+Left", "Ctrl+Shift+Enter", "Ctrl+Alt+Backspace"}) {
        CHECK(format(must_parse(text)) == text);
    }
}

TEST_CASE("hotkey: unusable bindings are refused with a reason", "[hotkey]") {
    for (const char* bad : {
             "",                 // nothing
             "Space",            // no modifier: would swallow typing
             "Shift+A",          // Shift alone is still typing
             "A",                //
             "Ctrl+Alt",         // no key
             "Ctrl+Alt+B+C",     // two keys
             "Ctrl++B",          // empty part
             "+",                //
             "Win+B",            // reserved for Windows
             "Ctrl+Win+B",       //
             "Ctrl+F12",         // reserved for debuggers
             "Alt+F4",           // closes windows
             "Alt+Tab",          // task switcher
             "Alt+Space",        // window menu
             "Ctrl+Alt+Delete",  // secure attention sequence
             "Ctrl+F25",         // no such key
             "Ctrl+F0",          //
             "Ctrl+Escape",      // not offered
             "Ctrl+;",           // punctuation is not offered
             "Ctrl+\xC3\xA9",    // non-ASCII
         }) {
        const ParseResult r = parse(bad);
        INFO("binding: '" << bad << "'");
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.error.empty());
    }
    CHECK_FALSE(parse(std::string(65, 'a')).ok());
    CHECK(parse("Ctrl+Alt+B\r\n").ok());   // surrounding whitespace is trimmed
}

TEST_CASE("hotkey manager: a free binding becomes active", "[hotkey]") {
    FakeRegistrar system;
    Manager manager(system);
    CHECK(manager.status() == Status::None);
    CHECK(manager.active().empty());
    CHECK(manager.active_id() == 0);

    CHECK(manager.apply("alt+shift+space") == Status::Active);
    CHECK(manager.active() == "Alt+Shift+Space");
    CHECK(manager.requested() == "Alt+Shift+Space");   // canonical spelling
    CHECK(manager.active_id() != 0);
    CHECK(manager.message().empty());
    CHECK(system.ours.size() == 1);
}

TEST_CASE("hotkey manager: a binding owned by another app is reported as taken", "[hotkey]") {
    FakeRegistrar system;
    system.taken_elsewhere = {must_parse("Alt+Shift+Space")};
    Manager manager(system);

    CHECK(manager.apply("Alt+Shift+Space") == Status::Taken);
    CHECK(manager.status() == Status::Taken);
    CHECK(manager.active().empty());        // nothing is live
    CHECK(manager.active_id() == 0);
    CHECK(manager.requested() == "Alt+Shift+Space");
    CHECK(manager.message().find("already used by another app") != std::string::npos);
    CHECK(system.ours.empty());
}

TEST_CASE("hotkey manager: a failed change keeps the working hotkey", "[hotkey]") {
    FakeRegistrar system;
    system.taken_elsewhere = {must_parse("Ctrl+Alt+K")};
    Manager manager(system);
    REQUIRE(manager.apply("Alt+Shift+Space") == Status::Active);
    const int original_id = manager.active_id();

    SECTION("the new binding is taken") {
        CHECK(manager.apply("Ctrl+Alt+K") == Status::Taken);
        CHECK(manager.message().find("Ctrl+Alt+K is already used") != std::string::npos);
        CHECK(manager.message().find("keeps using Alt+Shift+Space") != std::string::npos);
    }
    SECTION("the new binding is not valid") {
        CHECK(manager.apply("Win+K") == Status::Invalid);
        CHECK_FALSE(manager.message().empty());
    }
    SECTION("Windows refuses for another reason") {
        system.fail_everything = true;
        CHECK(manager.apply("Ctrl+Alt+J") == Status::Failed);
    }

    // In every case the original hotkey is still registered and still the active one.
    CHECK(manager.active() == "Alt+Shift+Space");
    CHECK(manager.active_id() == original_id);
    REQUIRE(system.ours.size() == 1);
    CHECK(system.ours.begin()->second == must_parse("Alt+Shift+Space"));
}

TEST_CASE("hotkey manager: changing registers the new binding before releasing the old", "[hotkey]") {
    FakeRegistrar system;
    Manager manager(system);
    REQUIRE(manager.apply("Alt+Shift+Space") == Status::Active);
    const int first_id = manager.active_id();
    system.calls.clear();

    CHECK(manager.apply("Ctrl+Alt+B") == Status::Active);
    CHECK(manager.active() == "Ctrl+Alt+B");
    CHECK(manager.active_id() != first_id);   // WM_HOTKEY for the old id must no longer match
    REQUIRE(system.calls.size() == 2);
    CHECK(system.calls[0] == "register Ctrl+Alt+B");
    CHECK(system.calls[1] == "unregister " + std::to_string(first_id));
    REQUIRE(system.ours.size() == 1);
    CHECK(system.ours.begin()->second == must_parse("Ctrl+Alt+B"));

    // And back again: ids are reused without ever leaving two registrations behind.
    CHECK(manager.apply("Alt+Shift+Space") == Status::Active);
    CHECK(system.ours.size() == 1);
    CHECK(manager.active_id() == first_id);
}

TEST_CASE("hotkey manager: re-applying the active binding does nothing", "[hotkey]") {
    FakeRegistrar system;
    Manager manager(system);
    REQUIRE(manager.apply("Alt+Shift+Space") == Status::Active);
    system.calls.clear();

    CHECK(manager.apply("shift+alt+space") == Status::Active);   // same binding, different spelling
    CHECK(system.calls.empty());
    CHECK(system.ours.size() == 1);
}

TEST_CASE("hotkey manager: a taken binding can be retried once it is free", "[hotkey]") {
    FakeRegistrar system;
    system.taken_elsewhere = {must_parse("Alt+Shift+Space")};
    Manager manager(system);
    REQUIRE(manager.apply("Alt+Shift+Space") == Status::Taken);

    system.taken_elsewhere.clear();   // the other app quit
    CHECK(manager.apply("Alt+Shift+Space") == Status::Active);
    CHECK(manager.active() == "Alt+Shift+Space");
}

TEST_CASE("hotkey manager: release and destruction unregister", "[hotkey]") {
    FakeRegistrar system;
    {
        Manager manager(system);
        REQUIRE(manager.apply("Alt+Shift+Space") == Status::Active);
        manager.release();
        CHECK(system.ours.empty());
        CHECK(manager.active().empty());
        CHECK(manager.active_id() == 0);

        REQUIRE(manager.apply("Ctrl+Alt+B") == Status::Active);
        CHECK(system.ours.size() == 1);
    }   // destroyed while a binding is live
    CHECK(system.ours.empty());
}

TEST_CASE("hotkey: status codes for the UI", "[hotkey]") {
    CHECK(std::string(to_code(Status::Active)) == "active");
    CHECK(std::string(to_code(Status::Taken)) == "taken");
    CHECK(std::string(to_code(Status::Invalid)) == "invalid");
    CHECK(std::string(to_code(Status::Failed)) == "failed");
    CHECK(std::string(to_code(Status::None)) == "none");
}
