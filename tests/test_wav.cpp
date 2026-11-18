#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/wav.h"

using namespace buddy::wav;

TEST_CASE("PCM16 WAV has a canonical header and samples", "[wav]") {
    const std::vector<std::int16_t> samples{0, 0x1234, -2};
    const Result result = encode_pcm16(samples, 16'000, 1);
    REQUIRE(result.ok());
    REQUIRE(result.bytes.size() == 50);
    CHECK(std::string(result.bytes.begin(), result.bytes.begin() + 4) == "RIFF");
    CHECK(std::string(result.bytes.begin() + 8, result.bytes.begin() + 12) == "WAVE");
    CHECK(std::string(result.bytes.begin() + 36, result.bytes.begin() + 40) == "data");
    CHECK(result.bytes[40] == 6);
    CHECK(result.bytes[41] == 0);
    CHECK(result.bytes[44] == 0x00);
    CHECK(result.bytes[45] == 0x00);
    CHECK(result.bytes[46] == 0x34);
    CHECK(result.bytes[47] == 0x12);
}

TEST_CASE("WAV rejects unsupported formats", "[wav]") {
    CHECK_FALSE(encode_pcm16({}, 7'999, 1).ok());
    CHECK_FALSE(encode_pcm16({}, 16'000, 3).ok());
}

namespace {

void set_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
}

std::vector<std::uint8_t> two_seconds() {
    const std::vector<std::int16_t> samples(16'000 * 2, 0);
    return encode_pcm16(samples, 16'000, 1).bytes;
}

}  // namespace

TEST_CASE("WAV duration is read from the header", "[wav]") {
    CHECK(duration_ms(two_seconds()) == 2'000);

    // Stereo at 48 kHz: 192,000 bytes a second.
    const std::vector<std::int16_t> stereo(48'000 * 2 * 3, 0);
    CHECK(duration_ms(encode_pcm16(stereo, 48'000, 2).bytes) == 3'000);

    // No samples is a valid, empty clip.
    CHECK(duration_ms(encode_pcm16({}, 16'000, 1).bytes) == 0);
}

TEST_CASE("WAV duration copes with streamed headers and extra chunks", "[wav]") {
    // A server that streams its reply cannot know the size up front and writes 0 or 0xFFFFFFFF.
    for (const std::uint32_t stated : {0u, 0xFFFFFFFFu}) {
        std::vector<std::uint8_t> streamed = two_seconds();
        set_u32(streamed, 4, stated);
        set_u32(streamed, 40, stated);
        CHECK(duration_ms(streamed) == 2'000);
    }

    // Another chunk (here an odd-sized one, so it is padded) may sit between format and audio.
    std::vector<std::uint8_t> with_list = two_seconds();
    const std::vector<std::uint8_t> list{'L', 'I', 'S', 'T', 3, 0, 0, 0, 'a', 'b', 'c', 0};
    with_list.insert(with_list.begin() + 36, list.begin(), list.end());
    CHECK(duration_ms(with_list) == 2'000);
}

TEST_CASE("WAV duration refuses anything that is not a readable WAVE file", "[wav]") {
    CHECK_FALSE(duration_ms({}).has_value());

    const std::vector<std::uint8_t> html{'<', 'h', 't', 'm', 'l', '>', 'n', 'o', 't', ' ', 'a', 'u', 'd', 'i', 'o'};
    CHECK_FALSE(duration_ms(html).has_value());

    std::vector<std::uint8_t> header_only = two_seconds();
    header_only.resize(36);   // ends before the audio chunk
    CHECK_FALSE(duration_ms(header_only).has_value());

    std::vector<std::uint8_t> no_rate = two_seconds();
    set_u32(no_rate, 28, 0);   // zero bytes per second
    CHECK_FALSE(duration_ms(no_rate).has_value());

    std::vector<std::uint8_t> bad_chunk = two_seconds();
    set_u32(bad_chunk, 16, 0x7FFFFFFF);   // the format chunk claims to be larger than the file
    CHECK_FALSE(duration_ms(bad_chunk).has_value());
}
