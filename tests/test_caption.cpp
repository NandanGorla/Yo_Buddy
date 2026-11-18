#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/caption.h"

using namespace buddy::caption;

TEST_CASE("caption: strips markdown for speech and overlay", "[caption]") {
    CHECK(to_plain("Hello world") == "Hello world");
    CHECK(to_plain("**Hello** _world_") == "Hello world");
    CHECK(to_plain("[Click](https://example.com) here") == "Click here");
    CHECK(to_plain("# Title\n- one\n- two") == "Title one two");
    CHECK(to_plain("Use `foo` please") == "Use foo please");
}

TEST_CASE("caption: fenced code is not an overlay caption", "[caption]") {
    const std::string reply = "Sure.\n```cpp\nint x = 1;\n```\n";
    CHECK_FALSE(overlay_ok(reply));
    CHECK(for_overlay(reply).empty());
    CHECK(for_speech(reply) == "Sure.");
}

TEST_CASE("caption: a table stays in the panel", "[caption]") {
    const std::string table = "A|B\n---|---\n1|2\n";
    CHECK_FALSE(overlay_ok(table));
    CHECK(for_overlay(table).empty());
}

TEST_CASE("caption: diagrams stay in the panel", "[caption][m4]") {
    CHECK_FALSE(overlay_ok("flowchart LR\nA --> B\n"));
    CHECK_FALSE(overlay_ok("graph TD\nA --> B\n"));
    CHECK_FALSE(overlay_ok("<svg><path d=\"M0 0\"/></svg>"));
    CHECK_FALSE(overlay_ok("Start --> Review --> Done"));
}

TEST_CASE("caption: a short plain reply is overlay-ready", "[caption]") {
    CHECK(overlay_ok("Click File, then New."));
    CHECK(for_overlay("Click **File**, then New.") == "Click File, then New.");
    CHECK(for_speech("Click **File**, then New.") == "Click File, then New.");
}

TEST_CASE("caption: a long reply is spoken but not captioned", "[caption]") {
    const std::string long_reply(kMaxOverlayBytes + 1, 'a');
    CHECK_FALSE(overlay_ok(long_reply));
    CHECK(for_overlay(long_reply).empty());
    CHECK(for_speech(long_reply).size() == kMaxOverlayBytes + 1);
}

TEST_CASE("caption: a code-only reply still has something to say", "[caption]") {
    CHECK(for_speech("```\nint x;\n```") == "The reply is in the chat panel.");
    CHECK(for_overlay("```\nint x;\n```").empty());
}

// Buddy tutors programming, so captions are full of identifiers and operators that happen to
// use Markdown's emphasis characters. They must come through unchanged.
TEST_CASE("caption: code identifiers and operators are not treated as emphasis", "[caption]") {
    CHECK(to_plain("A `std::unique_ptr` owns it; `size_t` is unsigned.") ==
          "A std::unique_ptr owns it; size_t is unsigned.");
    CHECK(to_plain("Write `int* p = &x;` and then `*p` reads x.") == "Write int* p = &x; and then *p reads x.");
    CHECK(to_plain("snake_case_name and my_var") == "snake_case_name and my_var");
    CHECK(to_plain("2 * 3 * 4 is 24") == "2 * 3 * 4 is 24");
    CHECK(to_plain("int *p, *q;") == "int *p, *q;");
    CHECK(to_plain("*not closed") == "*not closed");
    CHECK(to_plain("#include <vector>") == "#include <vector>");
    CHECK(to_plain("Use ``a ` b`` here") == "Use a ` b here");
}

TEST_CASE("caption: real emphasis, headings, quotes and images are still stripped", "[caption]") {
    CHECK(to_plain("**bold *and* more**") == "bold and more");
    CHECK(to_plain("***both***") == "both");
    CHECK(to_plain("__bold__ and _italic_") == "bold and italic");
    CHECK(to_plain("### Step 1\n> Click **Run**.") == "Step 1 Click Run.");
    CHECK(to_plain("![logo](logo.png) Hi") == "logo Hi");
    CHECK(to_plain("See [the **docs**](https://example.com).") == "See the docs.");
    CHECK(to_plain("+ one\n* two\n- three") == "one two three");
}

TEST_CASE("caption: an indented code fence is skipped like any other", "[caption]") {
    // Models indent fences inside list items.
    const std::string reply = "1. Do this:\n   ```cpp\n   int x = 1;\n   ```\nDone.";
    CHECK(to_plain(reply) == "1. Do this: Done.");
    CHECK(for_speech(reply) == "1. Do this: Done.");
    CHECK(to_plain("Before\n~~~\ncode\n~~~\nAfter") == "Before After");
    CHECK(to_plain("Before\n```\nnever closed") == "Before");
}

TEST_CASE("caption: a huge reply is handled quickly and never captioned", "[caption]") {
    // Unmatched markers everywhere: this must not take time proportional to the square of the size.
    std::string huge;
    while (huge.size() < 200'000) huge += "*a _b ";
    CHECK_FALSE(overlay_ok(huge));
    CHECK(for_speech(huge).size() <= kMaxSpeechBytes);
    CHECK_FALSE(for_speech(huge).empty());
}
