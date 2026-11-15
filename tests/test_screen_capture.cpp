#include <catch2/catch_test_macros.hpp>

#include "platform/screen_capture.h"

using namespace buddy::capture;

TEST_CASE("screen capture: invalid target is rejected before capture", "[capture]") {
    const FrameResult result = capture_one(TargetResult{{}, nullptr, "invalid target"});
    CHECK_FALSE(result.ok());
    CHECK(result.error == "invalid target");
}

TEST_CASE("screen capture: invalid size limits are rejected", "[capture]") {
    const TargetResult target{{}, GetDesktopWindow(), {}};

    const FrameResult too_small = capture_one(target, 63);
    CHECK_FALSE(too_small.ok());
    CHECK(too_small.error == "Capture size limit is invalid.");

    const FrameResult too_large = capture_one(target, 8193);
    CHECK_FALSE(too_large.ok());
    CHECK(too_large.error == "Capture size limit is invalid.");
}

TEST_CASE("screen capture: invalid frames are rejected by the PNG encoder", "[capture]") {
    const EncodedFrameResult result = encode_png(Frame{1, 1, 3, {0, 0, 0}});
    CHECK_FALSE(result.ok());
    CHECK(result.error == "The captured frame is invalid.");
}
