#include "domain/arm/interpreter/interpreter.h"
#include "domain/lift/engine.h"
#include "domain/lift/runtime.h"
#include "domain/memory/process_memory.h"

#include "sample_code.h"

#include <array>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hexlith::domain::lift::sample_lifted {
void Register(runtime::EntryTable& table);
}  // namespace hexlith::domain::lift::sample_lifted

namespace hexlith::domain::lift::sample_lifted_module {
void Register(runtime::EntryTable& table);
}  // namespace hexlith::domain::lift::sample_lifted_module

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
    // With `patched`, the word the sample marks as relocated holds what a loader
    // would have written there and not what the file says. With a `base`, the
    // sample is a module mapped there, and the loader has also made the three
    // addresses of its jump table absolute.
    World(bool patched, std::uint32_t base) {
        const std::shared_ptr<Backing> code = Backing::Allocate(2 * Page);
        for (std::size_t index = 0; index < sample::Code.size(); ++index) {
            std::uint32_t word = sample::Code.at(index);
            const bool in_table =
                index >= sample::TableIndex && index < sample::TableIndex + sample::TableEntries;
            if (in_table) {
                word += base;
            }
            for (unsigned byte = 0; byte < 4; ++byte) {
                code->Bytes()[(index * 4) + byte] = static_cast<std::uint8_t>(word >> (8U * byte));
            }
        }
        if (patched) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                code->Bytes()[(sample::RelocatedIndex * 4) + byte] =
                    static_cast<std::uint8_t>(sample::RelocatedMemoryWord >> (8U * byte));
            }
        }
        EXPECT_TRUE(memory.Map(base + sample::Base, 2 * Page, code, 0, Access::ReadExecute));
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
    bool patched = false;

    // Where the module is, or zero for the main executable. The entry is an
    // address in the running process either way.
    std::uint32_t base = 0;
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
    state.registers.at(arm::LinkRegister) = start.base + sample::At(sample::SentinelIndex);
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
std::string Disagreement(const Engine& engine, const Start& start, std::uint64_t limit) {
    World reference{start.patched, start.base};
    World lifted{start.patched, start.base};
    Prepare(reference, start);
    Prepare(lifted, start);

    std::uint64_t expected_count = 0;
    const arm::Stop expected = arm::Run(reference.state, reference.memory, limit, expected_count);

    std::uint64_t count = 0;
    const arm::Stop stop = engine.Run(lifted.state, lifted.memory, limit, count);

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

std::string
Disagreement(const runtime::EntryTable& table, const Start& start, std::uint64_t limit) {
    return Disagreement(Engine{table}, start, limit);
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

// The lifter reads the file, where a relocation has not yet been applied, so an
// instruction at a relocated word must come from memory instead.
TEST(GeneratedCode, ARelocatedWordRunsAsMemoryHasItAndNotAsTheFileHasIt) {
    const runtime::EntryTable table = SampleTable();
    const Start start{.entry = sample::At(sample::PatchedIndex), .patched = true};
    EXPECT_EQ(Disagreement(table, start, 100000), "");
}

TEST(GeneratedCode, StoppingInAFunctionWithARelocatedWordAgreesWithTheInterpreter) {
    const runtime::EntryTable table = SampleTable();
    for (std::uint64_t limit = 1; limit < 8; ++limit) {
        const Start start{.entry = sample::At(sample::PatchedIndex), .patched = true};
        EXPECT_EQ(Disagreement(table, start, limit), "") << "limit " << limit;
    }
}

TEST(GeneratedCode, StartingInTheMiddleOfAFunctionWorks) {
    const runtime::EntryTable table = SampleTable();
    // The block after the call in the caller, as the engine resumes there after
    // the kernel has been in the way.
    const Start start{.entry = sample::At(11), .r0 = 55};
    EXPECT_EQ(Disagreement(table, start, 100000), "");
}

// The same functions lifted as a module's, entered through what the loader says is
// loaded, at two of the addresses one module was loaded at in a run of the title.
// The interpreter runs the same bytes in the same place.
constexpr std::array<std::uint32_t, 2> ModuleBases = {0x00400000, 0x00605000};
constexpr std::string_view SampleName = "Sample";
constexpr std::string_view SampleDigest = "cc";

class LoadedSample {
public:
    explicit LoadedSample(std::uint32_t base) : base_(base), engine_(none_, &modules_) {
        sample_lifted_module::Register(table_);
        modules_.Offer(SampleName, SampleDigest, table_);
        EXPECT_EQ(modules_.Loaded(SampleName,
                                  base,
                                  sample::Base,
                                  static_cast<std::uint32_t>(sample::Code.size() * 4),
                                  SampleDigest),
                  runtime::LoadedModules::Outcome::Lifted);
    }

    [[nodiscard]] std::string Disagrees(std::uint32_t function, Start start, std::uint64_t limit) {
        start.entry = base_ + sample::At(function);
        start.base = base_;
        return Disagreement(engine_, start, limit);
    }

    [[nodiscard]] const Engine& TheEngine() const {
        return engine_;
    }

private:
    std::uint32_t base_;
    runtime::EntryTable none_;
    runtime::EntryTable table_;
    runtime::LoadedModules modules_;
    Engine engine_;
};

std::string BaseName(std::uint32_t base) {
    return std::format("base {:#x}", base);
}

// Agreeing would be as true of an engine that never left the interpreter, so the
// runs have to be seen to start in the module's lifted code.
TEST(GeneratedModuleCode, ALoopAgreesWithTheInterpreterAtTwoBases) {
    constexpr std::uint64_t Runs = 4;
    for (const std::uint32_t base : ModuleBases) {
        LoadedSample module{base};
        for (const std::uint32_t count : {0U, 1U, 5U, 100U}) {
            EXPECT_EQ(module.Disagrees(sample::SumIndex, {.r1 = count}, 100000), "")
                << BaseName(base) << " count " << count;
        }
        EXPECT_GE(module.TheEngine().Counted().entries, Runs) << BaseName(base);
    }
}

TEST(GeneratedModuleCode, ACallAndItsMemoryAgreeWithTheInterpreterAtTwoBases) {
    constexpr std::uint64_t Runs = 2;
    for (const std::uint32_t base : ModuleBases) {
        LoadedSample module{base};
        for (const bool zero : {false, true}) {
            EXPECT_EQ(module.Disagrees(sample::CallerIndex, {.zero = zero}, 100000), "")
                << BaseName(base);
        }
        EXPECT_GE(module.TheEngine().Counted().entries, Runs) << BaseName(base);
    }
}

TEST(GeneratedModuleCode, ASwitchThroughRelocatedAddressesAgreesAtTwoBases) {
    for (const std::uint32_t base : ModuleBases) {
        LoadedSample module{base};
        for (const std::uint32_t index : {0U, 1U, 2U, 3U, 9U, 0xFFFFFFFFU}) {
            EXPECT_EQ(module.Disagrees(sample::SwitcherIndex, {.r0 = index}, 100000), "")
                << BaseName(base) << " index " << index;
        }
        for (const std::uint32_t index : {0U, 1U, 2U, 5U}) {
            EXPECT_EQ(module.Disagrees(sample::MixedIndex, {.r0 = index, .r1 = 3}, 100000), "")
                << BaseName(base) << " index " << index;
        }
    }
}

TEST(GeneratedModuleCode, ARelocatedWordRunsAsMemoryHasItAtTwoBases) {
    for (const std::uint32_t base : ModuleBases) {
        LoadedSample module{base};
        EXPECT_EQ(module.Disagrees(sample::PatchedIndex, {.patched = true}, 100000), "")
            << BaseName(base);
    }
}

// Stopping after any number of instructions has to leave the processor where the
// interpreter would, wherever in a block that falls.
void ExpectEveryStopAgrees(LoadedSample& module, std::uint32_t function, std::uint32_t base) {
    for (std::uint64_t limit = 1; limit < 70; ++limit) {
        EXPECT_EQ(module.Disagrees(function, {.r0 = 1, .r1 = 6, .patched = true}, limit), "")
            << "function " << function << " limit " << limit << " " << BaseName(base);
    }
}

TEST(GeneratedModuleCode, StoppingAfterAnyNumberOfInstructionsAgreesAtTwoBases) {
    for (const std::uint32_t base : ModuleBases) {
        LoadedSample module{base};
        for (const std::uint32_t function : {sample::SumIndex,
                                             sample::CallerIndex,
                                             sample::SwitcherIndex,
                                             sample::MixedIndex,
                                             sample::PatchedIndex}) {
            ExpectEveryStopAgrees(module, function, base);
        }
    }
}

TEST(GeneratedModuleCode, AModuleThatIsNotLoadedIsInterpretedAndStillAgrees) {
    const runtime::EntryTable none;
    runtime::EntryTable table;
    sample_lifted_module::Register(table);
    runtime::LoadedModules modules;
    modules.Offer(SampleName, SampleDigest, table);
    const Engine engine{none, &modules};
    Start start{.entry = ModuleBases.front() + sample::At(sample::SumIndex), .r1 = 5};
    start.base = ModuleBases.front();
    EXPECT_EQ(Disagreement(engine, start, 100000), "");
    EXPECT_EQ(engine.Counted().entries, 0U);
}

}  // namespace
}  // namespace hexlith::domain::lift
