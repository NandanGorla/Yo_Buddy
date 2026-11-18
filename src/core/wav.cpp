#include "core/wav.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace buddy::wav {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

Result fail(std::string error) { return {{}, std::move(error)}; }

}  // namespace

Result encode_pcm16(std::span<const std::int16_t> samples, int sample_rate, int channels) {
    if (sample_rate < 8'000 || sample_rate > 192'000) return fail("unsupported sample rate");
    if (channels < 1 || channels > 2) return fail("unsupported channel count");
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 44u) / sizeof(std::int16_t)) {
        return fail("audio payload is too large");
    }

    const auto data_size = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const auto byte_rate = static_cast<std::uint32_t>(sample_rate * channels * sizeof(std::int16_t));
    const auto block_align = static_cast<std::uint16_t>(channels * sizeof(std::int16_t));
    std::vector<std::uint8_t> out;
    out.reserve(44u + data_size);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    put_u32(out, 36u + data_size);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(out, 16);
    put_u16(out, 1);
    put_u16(out, static_cast<std::uint16_t>(channels));
    put_u32(out, static_cast<std::uint32_t>(sample_rate));
    put_u32(out, byte_rate);
    put_u16(out, block_align);
    put_u16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    put_u32(out, data_size);
    for (const std::int16_t sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        put_u16(out, value);
    }
    return {std::move(out), {}};
}

std::optional<std::uint64_t> duration_ms(std::span<const std::uint8_t> file) {
    const auto tag_at = [&](std::size_t offset, std::string_view tag) {
        return std::equal(tag.begin(), tag.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
    };
    const auto u32_at = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(file[offset]) | (static_cast<std::uint32_t>(file[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(file[offset + 2]) << 16) | (static_cast<std::uint32_t>(file[offset + 3]) << 24);
    };
    if (file.size() < 12 || !tag_at(0, "RIFF") || !tag_at(8, "WAVE")) return std::nullopt;

    // Walk the chunks: "fmt " gives the bytes per second, "data" the number of bytes.
    std::uint32_t bytes_per_second = 0;
    std::size_t offset = 12;
    while (file.size() - offset >= 8) {
        const std::size_t body = offset + 8;
        const std::size_t available = file.size() - body;
        const std::size_t stated = u32_at(offset + 4);
        if (tag_at(offset, "data")) {
            if (bytes_per_second == 0) return std::nullopt;   // no usable format before the audio
            const std::uint64_t audio_bytes = (stated == 0 || stated > available) ? available : stated;
            return audio_bytes * 1000u / bytes_per_second;
        }
        if (stated > available) return std::nullopt;          // cut off inside some other chunk
        if (tag_at(offset, "fmt ")) {
            if (stated < 16) return std::nullopt;
            bytes_per_second = u32_at(body + 8);
        }
        offset = body + stated + (stated & 1u);               // chunks are padded to an even size
        if (offset > file.size()) return std::nullopt;
    }
    return std::nullopt;   // no audio chunk at all
}

}  // namespace buddy::wav
