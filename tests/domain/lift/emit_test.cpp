#include "domain/analysis/tables.h"
#include "domain/lift/control_flow.h"
#include "domain/lift/emit.h"

#include "sample_code.h"

#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hexlith::domain::lift {
namespace {

namespace sample = test::sample;
namespace analysis = domain::analysis;

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

// The functions that call, switch and branch, which are the ones whose text
// holds the most addresses.
std::string EmitSampleFunctions(const EmitContext& base_context) {
    const std::vector<std::uint8_t> bytes = SampleBytes();
    const CodeImage image{.bytes = bytes, .base = sample::Base};
    const std::unordered_set<std::uint32_t> entries{sample::At(sample::SumIndex),
                                                    sample::At(sample::CallerIndex),
                                                    sample::At(sample::SwitcherIndex),
                                                    sample::At(sample::MixedIndex)};
    const std::vector<analysis::Region> regions = {
        analysis::Region{.address = sample::At(sample::TableIndex),
                         .size = sample::TableEntries * sample::Word,
                         .kind = analysis::RegionKind::JumpTable,
                         .instruction_set = std::nullopt,
                         .source = analysis::Provenance::Heuristic}};
    const FlowContext flow{.entries = &entries, .regions = regions};

    EmitContext context = base_context;
    context.function_at = [&entries](std::uint32_t address,
                                     arm::InstructionSet set) -> std::optional<std::string> {
        if (set == arm::InstructionSet::Arm && entries.contains(address)) {
            return DefaultFunctionName(address, set);
        }
        return std::nullopt;
    };
    std::string text;
    for (const std::uint32_t entry : entries) {
        const Function function = BuildFunction(image, entry, arm::InstructionSet::Arm, flow);
        text +=
            EmitFunction(function, DefaultFunctionName(entry, arm::InstructionSet::Arm), context)
                .text;
    }
    return text;
}

// A constant that is an address of the code can only be right for one place the
// module is loaded. Entering a block (a case label) is the exception: it is
// switched on the offset.
TEST(EmitForAModule, NoAddressOfTheCodeIsWrittenWithoutTheBase) {
    EmitContext context;
    context.module = true;
    const std::string text = EmitSampleFunctions(context);

    std::size_t found = 0;
    for (std::uint32_t address = sample::Base; address < sample::At(sample::SentinelIndex);
         address += 1) {
        const std::string constant = std::format("0x{:08X}U", address);
        for (std::size_t at = text.find(constant); at != std::string::npos;
             at = text.find(constant, at + 1)) {
            ++found;
            const bool with_base = at >= 9 && text.compare(at - 9, 9, "c.base + ") == 0;
            const bool as_case = at >= 5 && text.compare(at - 5, 5, "case ") == 0;
            EXPECT_TRUE(with_base || as_case)
                << constant << " is written bare: " << text.substr(at - 40, 80);
        }
    }
    EXPECT_GT(found, 20U) << "the functions were expected to hold many addresses";
}

TEST(EmitForAModule, TheEntrySwitchAndTheSwitchExitWorkOnOffsets) {
    EmitContext context;
    context.module = true;
    const std::string text = EmitSampleFunctions(context);
    EXPECT_NE(text.find("switch (pc - c.base)"), std::string::npos);
    EXPECT_NE(text.find("switch (cpu.registers[15] - c.base)"), std::string::npos);
    EXPECT_EQ(text.find("switch (pc)"), std::string::npos);
}

TEST(EmitForAModule, TheMainExecutableIsEmittedWithoutABase) {
    const std::string text = EmitSampleFunctions(EmitContext{});
    EXPECT_EQ(text.find("c.base"), std::string::npos);
    EXPECT_NE(text.find("switch (pc)"), std::string::npos);
}

}  // namespace
}  // namespace hexlith::domain::lift
