#include "domain/gpu/recording_index.h"

#include <gtest/gtest.h>
#include <string>
#include <variant>

namespace hexlith::domain::gpu {
namespace {

constexpr std::string_view Digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

TEST(ParseRecordingIndex, ReadsEveryKindOfEvent) {
    const std::string text =
        "# hexlith gpu recording, format 2\n"
        "0 gx 00000001 14000000 00000010 00000000 00000000 00000000 00000000 00000000\n"
        "0 page 1F000000 " +
        std::string{Digest} +
        "\n"
        "0 list 14000000 00000010 000000.bin\n"
        "3 framebuffer 1 00000001 1F000000 00000000 000002D0 00000301 00000001 00000000\n";
    const core::Result<RecordingIndex> index = ParseRecordingIndex(text);
    ASSERT_TRUE(index.Ok()) << index.Failure().Message();
    EXPECT_EQ(index.Value().format, 2U);
    ASSERT_EQ(index.Value().events.size(), 4U);
    EXPECT_EQ(std::get<RecordedCommand>(index.Value().events[0]).words.at(1), 0x14000000U);
    EXPECT_EQ(std::get<RecordedPage>(index.Value().events[1]).digest, Digest);
    EXPECT_EQ(std::get<RecordedList>(index.Value().events[2]).size, 0x10U);
    const auto& framebuffer = std::get<RecordedFramebuffer>(index.Value().events[3]);
    EXPECT_EQ(framebuffer.frame, 3U);
    EXPECT_EQ(framebuffer.screen, 1U);
    EXPECT_EQ(framebuffer.words.at(3), 0x2D0U);
}

TEST(ParseRecordingIndex, ReadsFormatOne) {
    const core::Result<RecordingIndex> index =
        ParseRecordingIndex("# hexlith gpu recording, format 1\n0 list 14000000 00000010 a.bin\n");
    ASSERT_TRUE(index.Ok());
    EXPECT_EQ(index.Value().format, 1U);
}

TEST(ParseRecordingIndex, RefusesWhatItCannotRead) {
    const std::string header = "# hexlith gpu recording, format 2\n";
    for (const std::string& text : {
             std::string{"# hexlith gpu recording, format 3\n"},
             std::string{"0 list 14000000 00000010 a.bin\n"},
             header + "0 page 1F000000 ABCDEF\n",
             header + "0 list 14000000 00000010 ../escape.bin\n",
             header + "0 framebuffer 2 0 0 0 0 0 0 0\n",
             header + "0 gx 1 2 3\n",
             header + "5 list 0 1 a.bin\n4 list 0 1 b.bin\n",
             std::string{"# hexlith gpu recording, format 1\n0 framebuffer 0 0 0 0 0 0 0 0\n"},
         }) {
        EXPECT_FALSE(ParseRecordingIndex(text).Ok()) << text;
    }
}

}  // namespace
}  // namespace hexlith::domain::gpu
