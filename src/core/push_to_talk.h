#pragma once

#include <cstdint>

namespace buddy::voice {

// What the voice shortcut does, as plain logic with no Windows calls so it can be unit-tested.
//
//  - Hold the shortcut, speak, release: push-to-talk. The release ends the recording.
//  - Tap the shortcut (released again within kTapMs): the recording stays on until the next
//    press. This is the toggle fallback. It also covers the case where key state cannot be read
//    (an elevated window has the keyboard, so every key looks released at once).
//
// The owner calls on_press() for each shortcut press, on_poll() on a timer while recording, and
// reset() when the recording ended some other way (cancelled, length limit, device failure).
class PushToTalk {
public:
    enum class Action { None, Start, Stop, Latch };

    static constexpr std::uint64_t kTapMs = 350;

    Action on_press(std::uint64_t now_ms) {
        if (mode_ == Mode::Off) {
            mode_ = Mode::Held;
            pressed_at_ = now_ms;
            return Action::Start;
        }
        mode_ = Mode::Off;
        return Action::Stop;
    }

    Action on_poll(std::uint64_t now_ms, bool shortcut_down) {
        if (mode_ != Mode::Held || shortcut_down) return Action::None;
        if (now_ms - pressed_at_ < kTapMs) {
            mode_ = Mode::Latched;   // a tap: keep recording until the next press
            return Action::Latch;
        }
        mode_ = Mode::Off;
        return Action::Stop;
    }

    void reset() { mode_ = Mode::Off; }

    bool recording() const { return mode_ != Mode::Off; }
    bool latched() const { return mode_ == Mode::Latched; }

private:
    enum class Mode { Off, Held, Latched };
    Mode mode_ = Mode::Off;
    std::uint64_t pressed_at_ = 0;
};

}  // namespace buddy::voice
