#include "domain/lift/control_flow.h"
#include "domain/lift/emit.h"

#include "sample_code.h"

#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace hexlith::domain::lift {
namespace {

namespace sample = test::sample;

std::vector<std::uint8_t> SampleBytes() {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : sample::Code) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<std::uint8_t>(word >> (8U * index)));
        }
    }
    return bytes;
}

std::string EmitPatched(const EmitContext& context) {
    const std::vector<std::uint8_t> bytes = SampleBytes();
    const CodeImage image{.bytes = bytes, .base = sample::Base};
    const std::unordered_set<std::uint32_t> entries{sample::At(sample::PatchedIndex)};
    const FlowContext flow{.entries = &entries, .regions = {}};
    const Function function =
        BuildFunction(image, sample::At(sample::PatchedIndex), arm::InstructionSet::Arm, flow);
    return EmitFunction(function, "patched", context).text;
}

TEST(EmitRelocated, AnInstructionAtARelocatedWordGoesBackToTheInterpreter) {
    EmitContext context;
    context.relocated = [](std::uint32_t address) {
        return address == sample::At(sample::RelocatedIndex);
    };
    const std::string text = EmitPatched(context);

    // The block has four instructions and this is the second, so three have not
    // run when it is reached and the clock is given them back.
    const std::string bail =
        std::format("return runtime::Bail(c, 0x{:08X}U, 3);", sample::At(sample::RelocatedIndex));
    EXPECT_NE(text.find(bail), std::string::npos) << text;
    EXPECT_EQ(text.find("{0x00000002U,"), std::string::npos)
        << "the immediate the file holds was lifted anyway";
}

TEST(EmitRelocated, WithoutTheAnswerTheWordIsLiftedFromTheFile) {
    const std::string text = EmitPatched(EmitContext{});
    EXPECT_EQ(
        text.find(std::format("runtime::Bail(c, 0x{:08X}U", sample::At(sample::RelocatedIndex))),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("{0x00000002U,"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::domain::lift
