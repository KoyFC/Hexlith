#include "application/command_line.h"
#include "application/lift_verification.h"
#include "domain/lift/engine.h"
#include "domain/lift/runtime.h"

#include <array>
#include <format>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

namespace hexlith::application {
namespace {

namespace kernel = domain::kernel;
namespace lift = domain::lift;
using domain::memory::Access;

constexpr std::uint32_t Page = kernel::layout::PageSize;
constexpr std::uint32_t CodeAddress = 0x00100000;
constexpr std::uint32_t StackSize = 4 * Page;
constexpr std::uint32_t RegionSize = 64 * Page;
constexpr std::uint32_t MainPriority = 0x30;
constexpr std::uint32_t RunFast = 0x03000000;
constexpr std::uint32_t InstructionSize = 4;

// mov r0, #1; mov r1, #2; mov r2, #3; mov r3, #4; then b . for ever. The
// encodings are the ARMv6 reference manual's MOV (immediate) and B.
constexpr std::array<std::uint32_t, 5> Program = {
    0xE3A00001, 0xE3A01002, 0xE3A02003, 0xE3A03004, 0xEAFFFFFE};
constexpr unsigned ThirdRegister = 2;
constexpr std::uint32_t ThirdAddress = CodeAddress + (2 * InstructionSize);
constexpr std::uint32_t ThirdValue = 3;
constexpr std::uint32_t WrongValue = 99;

// Lifted code for the third instruction alone, as a block: it charges its one
// instruction when there is one left, sets r2 and says where the guest goes
// on. The wrong one sets r2 to something else.
template<std::uint32_t Value>
lift::runtime::Pc ThirdInstruction(lift::runtime::Context& context, lift::runtime::Pc /*pc*/) {
    if (context.budget < 1) {
        return lift::runtime::Interpret;
    }
    --context.budget;
    context.cpu->registers.at(ThirdRegister) = Value;
    return ThirdAddress + InstructionSize;
}

// A kernel running the program from its first word, through lifted code that
// covers the third instruction or through the interpreter alone.
class SyntheticRun final : public VerifiedRun {
public:
    SyntheticRun(bool lifted, lift::runtime::Function third)
        : kernel_(kernel::KernelConfig{
              .regions = {.application = RegionSize, .system = RegionSize, .base = RegionSize}}),
          engine_(entries_) {
        kernel::AddressSpace& space = kernel_.Space();
        std::vector<std::uint8_t> code;
        for (const std::uint32_t word : Program) {
            for (unsigned byte = 0; byte < InstructionSize; ++byte) {
                code.push_back(static_cast<std::uint8_t>(word >> (8U * byte)));
            }
        }
        EXPECT_EQ(
            space.CommitFixed(CodeAddress, Page, Access::ReadExecute, kernel::MemoryState::Code),
            kernel::result::Success);
        EXPECT_TRUE(space.Memory().Load(CodeAddress, code));
        EXPECT_EQ(space.CommitFixed(kernel::layout::HeapEnd - StackSize,
                                    StackSize,
                                    Access::ReadWrite,
                                    kernel::MemoryState::Locked),
                  kernel::result::Success);
        kernel_.Start(kernel::MainThreadStart{.entry_point = CodeAddress,
                                              .stack_top = kernel::layout::HeapEnd,
                                              .fpscr = RunFast,
                                              .priority = MainPriority});
        entries_.Add(ThirdAddress, false, third);
        if (lifted) {
            kernel_.UseLiftedCode(&engine_);
        }
    }

    kernel::Kernel& Kernel() override {
        return kernel_;
    }

private:
    kernel::Kernel kernel_;
    lift::runtime::EntryTable entries_;
    lift::Engine engine_;
};

VerifiedRunMaker MakerWith(lift::runtime::Function third) {
    return [third](bool lifted) -> core::Result<std::unique_ptr<VerifiedRun>> {
        return std::unique_ptr<VerifiedRun>{std::make_unique<SyntheticRun>(lifted, third)};
    };
}

constexpr std::uint64_t Limit = 1000;
constexpr std::uint64_t Step = 100;

TEST(LiftVerification, FindsNothingWhenLiftedCodeDoesWhatTheInterpreterDoes) {
    const core::Result<LiftVerification> verified =
        VerifyLiftedCode(MakerWith(ThirdInstruction<ThirdValue>), Limit, Step);
    ASSERT_TRUE(verified) << verified.Failure().Message();
    EXPECT_FALSE(verified.Value().divergence.has_value());
    EXPECT_EQ(verified.Value().agreed, Limit);
}

TEST(LiftVerification, NarrowsAMistakeDownToTheInstruction) {
    const core::Result<LiftVerification> verified =
        VerifyLiftedCode(MakerWith(ThirdInstruction<WrongValue>), Limit, Step);
    ASSERT_TRUE(verified) << verified.Failure().Message();
    ASSERT_TRUE(verified.Value().divergence.has_value());
    const LiftDivergence divergence = verified.Value().divergence.value_or(LiftDivergence{});
    EXPECT_EQ(divergence.instructions, 3U);
    EXPECT_EQ(divergence.last_agreed_pc, ThirdAddress);
    EXPECT_NE(divergence.difference.find("r2 is 00000003 interpreted and 00000063 lifted"),
              std::string::npos)
        << divergence.difference;
}

TEST(LiftVerification, ComparesMemoryAsWellAsRegisters) {
    SyntheticRun interpreted{false, ThirdInstruction<ThirdValue>};
    SyntheticRun lifted{true, ThirdInstruction<ThirdValue>};
    EXPECT_FALSE(DifferenceBetween(interpreted.Kernel(), lifted.Kernel()).has_value());

    // The top word of the stack, which the program never touches.
    constexpr std::uint32_t Changed = kernel::layout::HeapEnd - InstructionSize;
    ASSERT_TRUE(lifted.Kernel().Space().Memory().Write32(Changed, 1));
    const std::optional<std::string> difference =
        DifferenceBetween(interpreted.Kernel(), lifted.Kernel());
    const std::string described = difference.value_or("");
    EXPECT_NE(described.find(std::format("{:08X}", Changed)), std::string::npos) << described;
}

core::Result<LiftVerifyOptions> ParseVerify(std::vector<std::string_view> arguments) {
    return ParseLiftVerifyCommandLine(arguments);
}

TEST(ParseLiftVerifyCommandLine, TakesTheLibraryAndTheDefaults) {
    const core::Result<LiftVerifyOptions> parsed =
        ParseVerify({"--lifted", "lib.so", "--input", "script.txt"});
    ASSERT_TRUE(parsed) << parsed.Failure().Message();
    EXPECT_EQ(parsed.Value().lifted, "lib.so");
    EXPECT_EQ(parsed.Value().input, "script.txt");
    EXPECT_EQ(parsed.Value().instruction_limit, DefaultVerifyInstructionLimit);
    EXPECT_EQ(parsed.Value().step, DefaultVerifyStep);
}

TEST(ParseLiftVerifyCommandLine, TakesALimitAndAStep) {
    const core::Result<LiftVerifyOptions> parsed =
        ParseVerify({"--lifted", "lib.so", "--limit", "5000", "--step", "250"});
    ASSERT_TRUE(parsed) << parsed.Failure().Message();
    EXPECT_EQ(parsed.Value().instruction_limit, 5000U);
    EXPECT_EQ(parsed.Value().step, 250U);
}

TEST(ParseLiftVerifyCommandLine, RefusesNoLibraryAndAStepOfZero) {
    EXPECT_FALSE(ParseVerify({"--input", "script.txt"}));
    EXPECT_FALSE(ParseVerify({"--lifted", "lib.so", "--step", "0"}));
    EXPECT_TRUE(ParseVerify({"--help"}));
}

}  // namespace
}  // namespace hexlith::application
