#include "application/boot.h"
#include "application/command_line.h"

#include "support/synthetic_ctr_image.h"
#include "support/synthetic_romfs.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

namespace hexlith::application {
namespace {

namespace kernel = domain::kernel;

constexpr std::size_t ExecutablePartition = 0;
constexpr std::uint32_t Page = kernel::layout::PageSize;

// svc 0x03, ExitProcess, in ARM. Source: 3dbrew, SVC.
constexpr std::uint32_t ExitProcess = 0xEF000003;

// A title whose code exits at its first instruction, with a RomFS.
std::vector<std::uint8_t> ExitingTitle() {
    test::SyntheticImageSpec spec = test::DefaultImageSpec();
    const std::uint32_t pages = spec.text.pages + spec.read_only.pages + spec.data.pages;
    std::vector<std::uint8_t> code(static_cast<std::size_t>(pages) * Page, 0);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        code.at(byte) = static_cast<std::uint8_t>(ExitProcess >> (8U * byte));
    }
    spec.exefs_files = {test::ExeFsFileSpec{.name = ".code", .data = code}};
    spec.romfs = test::BuildSyntheticRomFs(test::DefaultRomFsSpec());
    return test::BuildSyntheticImage(spec).bytes;
}

TEST(BootedTitle, StartsAtTheEntryPointAndRunsUntilTheTitleExits) {
    const std::vector<std::uint8_t> image = ExitingTitle();
    core::Result<std::unique_ptr<BootedTitle>> title =
        BootedTitle::FromCartridge(image, ExecutablePartition);
    ASSERT_TRUE(title.Ok()) << title.Failure().Message();
    const kernel::KernelStop stop = title.Value()->Kernel().Run(Page);
    EXPECT_EQ(stop.reason, kernel::KernelStopReason::ProcessExited);
    EXPECT_EQ(title.Value()->Kernel().Executed(), 1U);
    EXPECT_EQ(title.Value()->Loader().Linked(), nullptr) << "nothing has asked for a module";
}

TEST(BootedTitle, RefusesAnImageWithoutAnExecutable) {
    constexpr std::size_t BannerSize = 0x200;
    test::SyntheticImageSpec spec = test::DefaultImageSpec();
    spec.exefs_files = {
        test::ExeFsFileSpec{.name = "banner", .data = std::vector<std::uint8_t>(BannerSize, 0)}};
    spec.romfs = test::BuildSyntheticRomFs(test::DefaultRomFsSpec());
    const std::vector<std::uint8_t> image = test::BuildSyntheticImage(spec).bytes;
    EXPECT_FALSE(BootedTitle::FromCartridge(image, ExecutablePartition).Ok());
}

TEST(CompareMemory, CountsTheBytesThatDiffer) {
    constexpr std::uint32_t Address = 0x00200000;
    domain::memory::ProcessMemory memory;
    ASSERT_TRUE(memory.Map(Address,
                           Page,
                           domain::memory::Backing::Allocate(Page),
                           0,
                           domain::memory::Access::ReadWrite));
    ASSERT_TRUE(memory.Write8(Address + 2, 1));
    const std::vector<std::uint8_t> expected = {0, 0, 0, 7};
    const MemoryComparison comparison = CompareMemory(memory, Address, expected);
    EXPECT_TRUE(comparison.readable);
    EXPECT_EQ(comparison.differing, 2U);
    EXPECT_EQ(comparison.first_difference, 2U);
}

TEST(CompareMemory, SaysWhenTheRangeIsNotMapped) {
    const domain::memory::ProcessMemory memory;
    const std::vector<std::uint8_t> expected(4, 0);
    EXPECT_FALSE(CompareMemory(memory, 0, expected).readable);
}

TEST(ParseBootCommandLine, TakesALimitAndADirectoryToCompareWith) {
    const std::vector<std::string_view> arguments = {"--limit",
                                                     "1000",
                                                     "--compare",
                                                     "/tmp/dump",
                                                     "--trace",
                                                     "--new-game",
                                                     "--golden",
                                                     "/tmp/g",
                                                     "--input",
                                                     "/tmp/i",
                                                     "--stop-after",
                                                     "cro-unload:DllA",
                                                     "--record-gpu"};
    const core::Result<BootOptions> options = ParseBootCommandLine(arguments);
    ASSERT_TRUE(options.Ok()) << options.Failure().Message();
    EXPECT_EQ(options.Value().instruction_limit, 1000U);
    EXPECT_EQ(options.Value().compare, "/tmp/dump");
    EXPECT_TRUE(options.Value().trace);
    EXPECT_TRUE(options.Value().new_game);
    EXPECT_EQ(options.Value().golden, "/tmp/g");
    EXPECT_EQ(options.Value().input, "/tmp/i");
    EXPECT_TRUE(options.Value().record_gpu);
    ASSERT_TRUE(options.Value().stop_after.has_value());
    EXPECT_TRUE(options.Value().stop_after.value_or(BootStopPoint{}).unload);
    EXPECT_EQ(options.Value().stop_after.value_or(BootStopPoint{}).module, "DllA");
}

TEST(ParseBootCommandLine, RunsLongEnoughByDefault) {
    const core::Result<BootOptions> options = ParseBootCommandLine({});
    ASSERT_TRUE(options.Ok());
    EXPECT_EQ(options.Value().instruction_limit, DefaultBootInstructionLimit);
}

TEST(ParseBootCommandLine, RefusesAStopPointItDoesNotKnow) {
    for (const std::string_view point : {"cro-load:", "frame:12", "DllA"}) {
        const std::vector<std::string_view> arguments = {"--stop-after", point};
        EXPECT_FALSE(ParseBootCommandLine(arguments).Ok()) << point;
    }
}

TEST(ParseBootCommandLine, RefusesALimitThatIsNotANumber) {
    const std::vector<std::string_view> arguments = {"--limit", "2e9"};
    EXPECT_FALSE(ParseBootCommandLine(arguments).Ok());
}

TEST(BootUsage, NamesEveryOption) {
    const std::string usage = BootUsage();
    for (const std::string_view option : {"--rom-dir",
                                          "--work-dir",
                                          "--image",
                                          "--limit",
                                          "--compare",
                                          "--trace",
                                          "--new-game",
                                          "--golden",
                                          "--input",
                                          "--stop-after",
                                          "--record-gpu",
                                          "--help"}) {
        EXPECT_NE(usage.find(option), std::string::npos) << option;
    }
}

}  // namespace
}  // namespace hexlith::application
