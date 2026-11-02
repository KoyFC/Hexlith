#include "domain/arm/interpreter/interpreter.h"
#include "domain/lift/engine.h"
#include "domain/lift/runtime.h"
#include "domain/memory/process_memory.h"

#include "sample_code.h"

#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace hexlith::domain::lift::sample_lifted {
void Register(runtime::EntryTable& table);
}  // namespace hexlith::domain::lift::sample_lifted

namespace hexlith::domain::lift {
namespace {

namespace sample = test::sample;
using memory::Access;
using memory::Backing;
using memory::ProcessMemory;

constexpr std::uint32_t Page = 0x1000;
constexpr std::uint32_t Data = 0x00200000;
constexpr std::uint32_t Stack = 0x00300000;

// The code, a page of data and a page of stack, mapped the way a process has
// them.
struct World {
    World() {
        const std::shared_ptr<Backing> code = Backing::Allocate(2 * Page);
        for (std::size_t index = 0; index < sample::Code.size(); ++index) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                code->Bytes()[(index * 4) + byte] =
                    static_cast<std::uint8_t>(sample::Code.at(index) >> (8U * byte));
            }
        }
        EXPECT_TRUE(memory.Map(sample::Base, 2 * Page, code, 0, Access::ReadExecute));
        EXPECT_TRUE(memory.Map(Data, Page, Backing::Allocate(Page), 0, Access::ReadWrite));
        EXPECT_TRUE(memory.Map(Stack, Page, Backing::Allocate(Page), 0, Access::ReadWrite));
    }

    arm::CpuState state;
    ProcessMemory memory;
};

struct Start {
    std::uint32_t entry = 0;
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
    bool zero = false;
    bool carry = false;
};

void Prepare(World& world, const Start& start) {
    arm::CpuState& state = world.state;
    state.registers.at(arm::ProgramCounter) = start.entry;
    state.registers.at(0) = start.r0;
    state.registers.at(1) = start.r1;
    state.registers.at(2) = 7;
    state.registers.at(3) = Data + 0x40;
    state.registers.at(4) = 0x44444444;
    state.registers.at(5) = 0x55555555;
    state.registers.at(arm::StackPointer) = Stack + 0x800;
    state.registers.at(arm::LinkRegister) = sample::At(sample::SentinelIndex);
    state.zero = start.zero;
    state.carry = start.carry;
    state.thread_id_read_only = 0xCAFE0003;
    state.float_registers.at(0) = 0x3F800000;
    state.float_registers.at(2) = 0x40000000;
}

std::string Differences(World& expected, World& actual) {
    for (std::size_t index = 0; index < expected.state.registers.size(); ++index) {
        if (expected.state.registers.at(index) != actual.state.registers.at(index)) {
            return std::format("r{} is {:#x}, the interpreter has {:#x}",
                               index,
                               actual.state.registers.at(index),
                               expected.state.registers.at(index));
        }
    }
    const arm::CpuState& e = expected.state;
    const arm::CpuState& a = actual.state;
    if (e.negative != a.negative || e.zero != a.zero || e.carry != a.carry ||
        e.overflow != a.overflow || e.thumb != a.thumb) {
        return "flags differ";
    }
    if (e.float_registers != a.float_registers) {
        return "floating point registers differ";
    }
    // The words the samples touch: the start of the data page, and the top of the
    // stack where the pushes go.
    for (const std::uint32_t base : {Data, Stack + 0x700}) {
        for (std::uint32_t offset = 0; offset < 0x100; offset += 4) {
            if (expected.memory.Read32(base + offset) != actual.memory.Read32(base + offset)) {
                return std::format("memory differs at {:#x}", base + offset);
            }
        }
    }
    return {};
}

// Runs the same start through the interpreter and through the engine, to the same
// limit, and says how they differ.
std::string
Disagreement(const runtime::EntryTable& table, const Start& start, std::uint64_t limit) {
    World reference;
    World lifted;
    Prepare(reference, start);
    Prepare(lifted, start);

    std::uint64_t expected_count = 0;
    const arm::Stop expected = arm::Run(reference.state, reference.memory, limit, expected_count);

    std::uint64_t count = 0;
    const arm::Stop stop = Engine{table}.Run(lifted.state, lifted.memory, limit, count);

    if (stop.reason != expected.reason || stop.detail != expected.detail) {
        return std::format("stops with {} {:#x}, the interpreter with {} {:#x}",
                           arm::Describe(stop.reason),
                           stop.detail,
                           arm::Describe(expected.reason),
                           expected.detail);
    }
    if (count != expected_count) {
        return std::format("executed {}, the interpreter {}", count, expected_count);
    }
    return Differences(reference, lifted);
}

runtime::EntryTable SampleTable() {
    runtime::EntryTable table;
    sample_lifted::Register(table);
    return table;
}

TEST(GeneratedCode, EveryBlockOfEverySampleFunctionCanBeEntered) {
    const runtime::EntryTable table = SampleTable();
    EXPECT_GT(table.Size(), 10U);
    EXPECT_NE(table.Find(sample::At(sample::SumIndex), false), nullptr);
    EXPECT_NE(table.Find(sample::At(3), false), nullptr) << "the body of the loop";
    EXPECT_EQ(table.Find(sample::At(sample::SentinelIndex), false), nullptr);
}

TEST(GeneratedCode, ALoopSumsAsTheInterpreterDoes) {
    const runtime::EntryTable table = SampleTable();
    for (const std::uint32_t count : {0U, 1U, 5U, 100U}) {
        const Start start{.entry = sample::At(sample::SumIndex), .r1 = count};
        EXPECT_EQ(Disagreement(table, start, 100000), "") << "count " << count;
    }
}

TEST(GeneratedCode, ACallReturnsToTheCallerAndMemoryIsKept) {
    const runtime::EntryTable table = SampleTable();
    for (const bool zero : {false, true}) {
        const Start start{.entry = sample::At(sample::CallerIndex), .zero = zero};
        EXPECT_EQ(Disagreement(table, start, 100000), "");
    }
}

TEST(GeneratedCode, ASwitchGoesWhereTheTableSays) {
    const runtime::EntryTable table = SampleTable();
    for (const std::uint32_t index : {0U, 1U, 2U, 3U, 9U, 0xFFFFFFFFU}) {
        const Start start{.entry = sample::At(sample::SwitcherIndex), .r0 = index};
        EXPECT_EQ(Disagreement(table, start, 100000), "") << "index " << index;
    }
}

TEST(GeneratedCode, CallsBlockTransfersAndInterpretedInstructionsTogether) {
    const runtime::EntryTable table = SampleTable();
    for (const std::uint32_t index : {0U, 1U, 2U, 5U}) {
        const Start start{.entry = sample::At(sample::MixedIndex), .r0 = index, .r1 = 3};
        EXPECT_EQ(Disagreement(table, start, 100000), "") << "index " << index;
    }
}

// The clock is the point: stopping after any number of instructions has to leave
// the processor exactly where the interpreter would have, wherever in a block
// that falls.
TEST(GeneratedCode, StoppingAfterAnyNumberOfInstructionsAgreesWithTheInterpreter) {
    const runtime::EntryTable table = SampleTable();
    for (const std::uint32_t entry : {sample::At(sample::SumIndex),
                                      sample::At(sample::CallerIndex),
                                      sample::At(sample::SwitcherIndex),
                                      sample::At(sample::MixedIndex)}) {
        for (std::uint64_t limit = 1; limit < 70; ++limit) {
            const Start start{.entry = entry, .r0 = 1, .r1 = 6};
            EXPECT_EQ(Disagreement(table, start, limit), "")
                << "entry " << std::hex << entry << " limit " << std::dec << limit;
        }
    }
}

TEST(GeneratedCode, StartingInTheMiddleOfAFunctionWorks) {
    const runtime::EntryTable table = SampleTable();
    // The block after the call in the caller, as the engine resumes there after
    // the kernel has been in the way.
    const Start start{.entry = sample::At(11), .r0 = 55};
    EXPECT_EQ(Disagreement(table, start, 100000), "");
}

}  // namespace
}  // namespace hexlith::domain::lift
