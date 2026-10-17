#include "application/input_script.h"

#include <gtest/gtest.h>

namespace hexlith::application {
namespace {

namespace services = domain::services;
namespace button = domain::services::button;

InputScript Parsed(std::string_view text) {
    core::Result<InputScript> script = InputScript::Parse(text);
    EXPECT_TRUE(script.Ok()) << script.Failure().Message();
    return std::move(script.Value());
}

TEST(InputScript, HoldsNothingBeforeItsFirstLine) {
    const InputScript script = Parsed("10 press a\n");
    EXPECT_EQ(script.StateAt(9), services::InputState{});
    EXPECT_EQ(script.StateAt(10).buttons, button::A);
    EXPECT_EQ(script.StateAt(1000).buttons, button::A) << "held until released";
}

TEST(InputScript, ReleasesWhatWasPressed) {
    const InputScript script = Parsed("10 press a b\n20 release a\n");
    EXPECT_EQ(script.StateAt(15).buttons, button::A | button::B);
    EXPECT_EQ(script.StateAt(20).buttons, button::B);
}

TEST(InputScript, LetsGoOfATapAfterItsLength) {
    const InputScript script = Parsed("10 tap start\n50 tap up for 2\n");
    EXPECT_EQ(script.StateAt(13).buttons, button::Start) << "four frames by default";
    EXPECT_EQ(script.StateAt(14).buttons, 0U);
    EXPECT_EQ(script.StateAt(51).buttons, button::Up);
    EXPECT_EQ(script.StateAt(52).buttons, 0U);
    EXPECT_EQ(script.LastFrame(), 52U);
}

TEST(InputScript, TouchesAndLifts) {
    const InputScript script = Parsed("5 touch 160 120 for 3\n20 touch 1 2\n30 lift\n");
    EXPECT_EQ(script.StateAt(5).touch, (services::TouchPoint{.x = 160, .y = 120}));
    EXPECT_FALSE(script.StateAt(8).touch.has_value());
    EXPECT_EQ(script.StateAt(25).touch, (services::TouchPoint{.x = 1, .y = 2}));
    EXPECT_FALSE(script.StateAt(30).touch.has_value());
}

TEST(InputScript, MovesTheCirclePad) {
    const InputScript script = Parsed("5 circle 154 -20\n9 circle 0 0\n");
    EXPECT_EQ(script.StateAt(6).circle_x, 154);
    EXPECT_EQ(script.StateAt(6).circle_y, -20);
    EXPECT_EQ(script.StateAt(9).circle_x, 0);
}

TEST(InputScript, LetsALaterLineWinAtTheSameFrame) {
    const InputScript script = Parsed("10 tap a for 4\n14 press a\n");
    EXPECT_EQ(script.StateAt(14).buttons, button::A)
        << "the tap lets go and the press holds it again";
}

TEST(InputScript, IgnoresCommentsAndBlankLines) {
    const InputScript script = Parsed("# a comment\n\n  7 press x   # held\n");
    EXPECT_EQ(script.StateAt(7).buttons, button::X);
}

TEST(InputScript, RefusesWhatItCannotRead) {
    for (const std::string_view text : {"x press a\n",
                                        "10 press\n",
                                        "10 press z\n",
                                        "10 jump\n",
                                        "10 touch 400 10\n",
                                        "10 circle 300 0\n",
                                        "10 tap a for 0\n",
                                        "10 press a extra\n",
                                        "20 press a\n10 press b\n"}) {
        EXPECT_FALSE(InputScript::Parse(text).Ok()) << text;
    }
}

TEST(InputScript, SaysWhichLineIsWrong) {
    const core::Result<InputScript> script = InputScript::Parse("1 press a\n2 press q\n");
    ASSERT_FALSE(script.Ok());
    EXPECT_NE(script.Failure().Message().find("line 2"), std::string::npos)
        << script.Failure().Message();
}

}  // namespace
}  // namespace hexlith::application
