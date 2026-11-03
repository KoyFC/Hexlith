#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"
#include "domain/lift/lower.h"

#include "differential.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <random>
#include <string>

namespace hexlith::domain::lift {
namespace {

using test::Machine;

// How many random instructions each test runs. The default keeps the suite
// quick; HEXLITH_LIFT_RANDOM_CASES raises it for the long runs that are the real
// check, and docs/lifter.md has the numbers those gave.
std::uint64_t CaseCount() {
    constexpr std::uint64_t Default = 4000;
    if (const char* text = std::getenv("HEXLITH_LIFT_RANDOM_CASES")) {
        return std::strtoull(text, nullptr, 10);
    }
    return Default;
}

// A register that points into the data page about half the time, so that
// loads and stores succeed, and holds small numbers or anything at all the rest.
std::uint32_t RandomRegister(std::mt19937_64& random) {
    constexpr unsigned Kinds = 4;
    constexpr std::uint32_t DataWindow = 0x800;
    constexpr std::uint32_t Small = 64;
    switch (random() % Kinds) {
    case 0:
        return test::Data + 0x400 + static_cast<std::uint32_t>(random() % DataWindow);
    case 1:
        return static_cast<std::uint32_t>(random() % Small);
    case 2:
        return test::Stack + 0x800 + static_cast<std::uint32_t>(random() % DataWindow);
    default:
        return static_cast<std::uint32_t>(random());
    }
}

test::Setup RandomState(std::uint64_t seed) {
    return [seed](Machine& machine) {
        std::mt19937_64 random{seed};
        test::DefaultSetup(machine);
        for (arm::RegisterIndex index = 0; index < arm::ProgramCounter; ++index) {
            machine.state.registers.at(index) = RandomRegister(random);
        }
        machine.state.negative = (random() & 1U) != 0;
        machine.state.zero = (random() & 1U) != 0;
        machine.state.carry = (random() & 1U) != 0;
        machine.state.overflow = (random() & 1U) != 0;
        machine.state.thread_id_read_write = static_cast<std::uint32_t>(random());
        machine.state.thread_id_read_only = static_cast<std::uint32_t>(random());
    };
}

TEST(LowerRandom, ArmWordsAgreeWithTheInterpreter) {
    // A fixed seed, so that a failure comes back on the next run.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 random{0x4C494654};
    const std::uint64_t count = CaseCount();
    std::uint64_t compared = 0;
    std::uint64_t translated = 0;
    for (std::uint64_t each = 0; each < count; ++each) {
        const arm::Instruction instruction = arm::DecodeArm(static_cast<std::uint32_t>(random()));
        if (!arm::IsDefined(instruction)) {
            continue;
        }
        ++compared;
        if (!Lower(PlacedInstruction{.address = test::Code, .instruction = instruction})
                 .IsInterpreted()) {
            ++translated;
        }
        const std::string difference = test::Disagreement(instruction, RandomState(random()));
        ASSERT_EQ(difference, "") << "word " << std::hex << instruction.encoding;
    }
    EXPECT_GT(compared, count / 4);
    EXPECT_GT(translated, compared / 8) << "the lowering should cover a share of what is defined";
}

TEST(LowerRandom, ThumbHalfwordsAgreeWithTheInterpreter) {
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 random{0x54485542};
    const std::uint64_t count = CaseCount();
    for (std::uint64_t each = 0; each < count; ++each) {
        const arm::Instruction instruction =
            arm::DecodeThumb(static_cast<std::uint16_t>(random()), 0);
        // A branch with link is two halfwords, and the first alone is not an
        // instruction a test can place.
        if (!arm::IsDefined(instruction) || instruction.width != 2) {
            continue;
        }
        const std::string difference = test::Disagreement(instruction, RandomState(random()));
        ASSERT_EQ(difference, "") << "halfword " << std::hex << instruction.encoding;
    }
}

// The same words as a module's code, which is lifted at offsets and runs at
// whatever address the game had it loaded at: two of the addresses one module
// was loaded at in one run of the title, docs/ldr-ro-trace.md. The interpreter
// executes at the address and the lowering is given the offset and the base, so
// an address the lowering got wrong shows only at the base it was not tested at.
constexpr std::uint32_t ModuleOffset = 0x1000;
constexpr std::array<std::uint32_t, 2> ModuleBases = {0x006A5000, 0x007E5000};

TEST(LowerRandomForAModule, ArmWordsAgreeWithTheInterpreterAtTwoBases) {
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 random{0x4D4F4455};
    const std::uint64_t count = CaseCount();
    for (std::uint64_t each = 0; each < count; ++each) {
        const arm::Instruction instruction = arm::DecodeArm(static_cast<std::uint32_t>(random()));
        if (!arm::IsDefined(instruction)) {
            continue;
        }
        const std::uint64_t seed = random();
        for (const std::uint32_t base : ModuleBases) {
            const test::Placement placement{.offset = ModuleOffset, .base = base};
            const std::string difference =
                test::Disagreement(instruction, RandomState(seed), placement);
            ASSERT_EQ(difference, "")
                << "word " << std::hex << instruction.encoding << " at base " << base;
        }
    }
}

TEST(LowerRandomForAModule, ThumbHalfwordsAgreeWithTheInterpreterAtTwoBases) {
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 random{0x4D4F4454};
    const std::uint64_t count = CaseCount();
    for (std::uint64_t each = 0; each < count; ++each) {
        const arm::Instruction instruction =
            arm::DecodeThumb(static_cast<std::uint16_t>(random()), 0);
        if (!arm::IsDefined(instruction) || instruction.width != 2) {
            continue;
        }
        const std::uint64_t seed = random();
        for (const std::uint32_t base : ModuleBases) {
            const test::Placement placement{.offset = ModuleOffset, .base = base};
            const std::string difference =
                test::Disagreement(instruction, RandomState(seed), placement);
            ASSERT_EQ(difference, "")
                << "halfword " << std::hex << instruction.encoding << " at base " << base;
        }
    }
}

}  // namespace
}  // namespace hexlith::domain::lift
