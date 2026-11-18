#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace buddy::caption {

// Overlay captions are short: they share the overlay.debug caption limit.
inline constexpr std::size_t kMaxOverlayBytes = 512;
// Spoken text is cut here, matching SapiVoice::kMaxSpokenBytes.
inline constexpr std::size_t kMaxSpeechBytes = 2'000;

// Markdown-ish reply text with emphasis, links and fences stripped, whitespace collapsed.
std::string to_plain(std::string_view markdown);

// True when the reply should appear as an overlay caption: short, no fenced code, no table.
bool overlay_ok(std::string_view markdown);

// Overlay caption text, or empty when overlay_ok is false.
std::string for_overlay(std::string_view markdown);

// Text to read aloud. Never empty when `markdown` has visible characters: a code-only reply
// becomes a short pointer at the chat panel.
std::string for_speech(std::string_view markdown);

}  // namespace buddy::caption
