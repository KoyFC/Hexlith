#include "application/gpu_replay.h"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

namespace hexlith::application {
namespace {

namespace gpu = domain::gpu;

// A recording held in memory: an index, and pages and lists by name.
class MemorySource final : public ports::GpuRecordingSource {
public:
    core::Result<std::string> Index() override {
        return index;
    }

    core::Result<std::vector<std::uint8_t>> Page(std::string_view digest) override {
        const auto found = pages.find(std::string{digest});
        if (found == pages.end()) {
            return core::Error{"no such page"};
        }
        return found->second;
    }

    core::Result<std::vector<std::uint8_t>> List(std::string_view file) override {
        const auto found = lists.find(std::string{file});
        if (found == lists.end()) {
            return core::Error{"no such list"};
        }
        return found->second;
    }

    [[nodiscard]] std::string Describe() const override {
        return "memory";
    }

    std::string index;
    std::map<std::string, std::vector<std::uint8_t>> pages;
    std::map<std::string, std::vector<std::uint8_t>> lists;
};

constexpr std::string_view First =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view Second =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::size_t PageSize = 0x1000;
// What the two pages hold, and where the page lies in FCRAM: 0x14001000 in the
// old linear heap.
constexpr std::uint8_t FirstFill = 0x11;
constexpr std::uint8_t SecondFill = 0x22;
constexpr std::uint32_t PagePhysical = 0x20001000;

// A list at frame 0 after a page of 0x11s in the linear heap, then a page of
// 0x22s over it and a second list at frame 2.
void FillTwoFrames(MemorySource& source) {
    source.index =
        "# hexlith gpu recording, format 2\n"
        "0 gx 00000001 14000000 00000004 00000000 00000000 00000000 00000000 00000000\n"
        "0 page 14001000 " +
        std::string{First} +
        "\n"
        "0 list 14000000 00000004 000000.bin\n"
        "1 framebuffer 0 00000000 1F000000 00000000 000002D0 00000301 00000000 00000000\n"
        "2 page 14001000 " +
        std::string{Second} +
        "\n"
        "2 list 14000000 00000004 000001.bin\n";
    source.pages[std::string{First}] = std::vector<std::uint8_t>(PageSize, FirstFill);
    source.pages[std::string{Second}] = std::vector<std::uint8_t>(PageSize, SecondFill);
    source.lists["000000.bin"] = std::vector<std::uint8_t>(4);
    source.lists["000001.bin"] = std::vector<std::uint8_t>(4);
}

// The byte of the recorded page that each list saw, in order.
std::vector<std::uint8_t> SeenByLists(MemorySource& source, std::optional<std::uint64_t> last) {
    gpu::GpuMemory memory;
    std::vector<std::uint8_t> seen;
    GpuReplayVisitor visitor;
    visitor.list = [&](const gpu::RecordedList&, std::span<const std::uint8_t>) {
        seen.push_back(memory.Bytes(PagePhysical, 1).front());
        return core::Status{};
    };
    EXPECT_TRUE(ReplayGpuRecording(source, memory, visitor, last).Ok());
    return seen;
}

TEST(ReplayGpuRecording, HandsEachListTheMemoryOfItsMoment) {
    MemorySource source;
    FillTwoFrames(source);
    EXPECT_EQ(SeenByLists(source, std::nullopt),
              (std::vector<std::uint8_t>{FirstFill, SecondFill}));
}

TEST(ReplayGpuRecording, StopsAfterTheLastFrameAskedFor) {
    MemorySource source;
    FillTwoFrames(source);
    EXPECT_EQ(SeenByLists(source, 1), (std::vector<std::uint8_t>{FirstFill}));
}

TEST(ReplayGpuRecording, HandsOnCommandsAndFramebuffers) {
    MemorySource source;
    FillTwoFrames(source);
    gpu::GpuMemory memory;
    std::vector<std::uint32_t> seen;
    GpuReplayVisitor visitor;
    visitor.command = [&](const gpu::RecordedCommand& command) {
        seen.push_back(command.words.front());
        return core::Status{};
    };
    visitor.framebuffer = [&](const gpu::RecordedFramebuffer& framebuffer) {
        seen.push_back(framebuffer.words.at(1));
        return core::Status{};
    };
    ASSERT_TRUE(ReplayGpuRecording(source, memory, visitor).Ok());
    EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 0x1F000000}));
}

TEST(ReplayGpuRecording, RefusesAListOfTheWrongSize) {
    MemorySource source;
    FillTwoFrames(source);
    source.lists["000001.bin"] = std::vector<std::uint8_t>(1);
    gpu::GpuMemory memory;
    EXPECT_FALSE(ReplayGpuRecording(source, memory, {}).Ok());
}

TEST(ReplayGpuRecording, RefusesAPageOutsideGpuMemory) {
    MemorySource source;
    FillTwoFrames(source);
    source.index =
        "# hexlith gpu recording, format 2\n0 page 08000000 " + std::string{First} + "\n";
    gpu::GpuMemory memory;
    EXPECT_FALSE(ReplayGpuRecording(source, memory, {}).Ok());
}

}  // namespace
}  // namespace hexlith::application
