#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace buddy::wav {

struct Result {
    std::vector<std::uint8_t> bytes;
    std::string error;

    bool ok() const { return !bytes.empty(); }
};

// A complete RIFF/WAVE file holding `samples` as 16-bit PCM.
Result encode_pcm16(std::span<const std::int16_t> samples, int sample_rate, int channels);

// How long a RIFF/WAVE file plays, in milliseconds. Empty when `file` is not a WAVE file whose
// header can be read. A data chunk whose stated size is zero or runs past the end of `file`
// (servers that stream audio write such headers) is taken to last until the end of `file`.
std::optional<std::uint64_t> duration_ms(std::span<const std::uint8_t> file);

}  // namespace buddy::wav
