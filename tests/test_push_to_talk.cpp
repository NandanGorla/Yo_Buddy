#include <catch2/catch_test_macros.hpp>

#include "core/push_to_talk.h"

using buddy::voice::PushToTalk;
using Action = PushToTalk::Action;

TEST_CASE("push to talk: holding records and releasing sends", "[voice][push_to_talk]") {
    PushToTalk ptt;
    CHECK(ptt.on_press(1'000) == Action::Start);
    CHECK(ptt.recording());

    // Polled while the shortcut is held: nothing happens, however long.
    CHECK(ptt.on_poll(1'016, true) == Action::None);
    CHECK(ptt.on_poll(9'000, true) == Action::None);
    CHECK_FALSE(ptt.latched());

    CHECK(ptt.on_poll(9'016, false) == Action::Stop);
    CHECK_FALSE(ptt.recording());
    CHECK(ptt.on_poll(9'032, false) == Action::None);   // already stopped
}

TEST_CASE("push to talk: a quick tap keeps recording until the next press", "[voice][push_to_talk]") {
    PushToTalk ptt;
    CHECK(ptt.on_press(1'000) == Action::Start);
    CHECK(ptt.on_poll(1'016, true) == Action::None);
    CHECK(ptt.on_poll(1'120, false) == Action::Latch);   // released after 120 ms: a tap
    CHECK(ptt.latched());

    // Hands off the keyboard, still recording.
    CHECK(ptt.on_poll(5'000, false) == Action::None);
    CHECK(ptt.recording());

    CHECK(ptt.on_press(6'000) == Action::Stop);
    CHECK_FALSE(ptt.recording());
}

TEST_CASE("push to talk: unreadable key state behaves like a tap", "[voice][push_to_talk]") {
    // With an elevated window in the foreground every key reads as up, from the very first poll.
    PushToTalk ptt;
    CHECK(ptt.on_press(1'000) == Action::Start);
    CHECK(ptt.on_poll(1'016, false) == Action::Latch);
    CHECK(ptt.latched());
    CHECK(ptt.on_press(4'000) == Action::Stop);
}

TEST_CASE("push to talk: the tap window has a sharp edge", "[voice][push_to_talk]") {
    PushToTalk just_inside;
    just_inside.on_press(0);
    CHECK(just_inside.on_poll(PushToTalk::kTapMs - 1, false) == Action::Latch);
    CHECK(just_inside.latched());

    PushToTalk at_the_edge;
    at_the_edge.on_press(0);
    CHECK(at_the_edge.on_poll(PushToTalk::kTapMs, false) == Action::Stop);
}

TEST_CASE("push to talk: a reset forgets the recording", "[voice][push_to_talk]") {
    // The recording ended some other way: cancelled, the length limit, a microphone failure.
    PushToTalk ptt;
    ptt.on_press(1'000);
    ptt.on_poll(1'050, false);
    REQUIRE(ptt.latched());

    ptt.reset();
    CHECK_FALSE(ptt.recording());
    CHECK(ptt.on_poll(2'000, false) == Action::None);
    CHECK(ptt.on_press(3'000) == Action::Start);   // the next press starts afresh, it does not "stop"
}
