#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/executed_addresses.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/memory/paged_memory.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::arm {
namespace {

constexpr std::uint32_t Code = 0x00100000;
constexpr std::uint32_t PageSize = memory::PagedMemory::PageSize;

// mov r0, #0 ; add r0, r0, #1 ; b back to the add
constexpr std::uint32_t MovZero = 0xE3A00000;
constexpr std::uint32_t AddOne = 0xE2800001;
constexpr std::uint32_t BranchBack = 0xEAFFFFFD;

// movs r0, #1 ; adds r0, #1 ; adds r0, #1
constexpr std::uint16_t ThumbMove = 0x2001;
constexpr std::uint16_t ThumbAdd = 0x3001;

void Load(memory::PagedMemory& memory, const std::vector<std::uint8_t>& bytes) {
    ASSERT_TRUE(memory.Map(Code, PageSize, false));
    ASSERT_TRUE(memory.Load(Code, bytes));
}

std::vector<std::uint8_t> ArmBytes(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t word : words) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(word >> shift));
        }
    }
    return bytes;
}

TEST(ExecutedAddresses, CountsEachInstructionOfALoop) {
    memory::PagedMemory memory;
    Load(memory, ArmBytes({MovZero, AddOne, BranchBack}));
    CpuState state;
    state.registers.at(ProgramCounter) = Code;

    ExecutedAddresses executed;
    std::uint64_t done = 0;
    const Stop stop = arm::Run(state, memory, 10, done, executed);

    EXPECT_EQ(stop.reason, StopReason::StepLimit);
    EXPECT_EQ(done, 10U);
    EXPECT_EQ(executed.Total(), 10U);
    EXPECT_EQ(executed.CountOf(Code, false), 1U);
    EXPECT_EQ(executed.CountOf(Code + 4, false), 5U);
    EXPECT_EQ(executed.CountOf(Code + 8, false), 4U);
    EXPECT_EQ(executed.CountOf(Code + 12, false), 0U);
}

TEST(ExecutedAddresses, ListsEntriesInAddressOrder) {
    memory::PagedMemory memory;
    Load(memory, ArmBytes({MovZero, AddOne, BranchBack}));
    CpuState state;
    state.registers.at(ProgramCounter) = Code;

    ExecutedAddresses executed;
    std::uint64_t done = 0;
    static_cast<void>(arm::Run(state, memory, 4, done, executed));

    const std::vector<ExecutedAddresses::Entry> entries = executed.Entries();
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries.at(0).address, Code);
    EXPECT_EQ(entries.at(1).address, Code + 4);
    EXPECT_EQ(entries.at(2).address, Code + 8);
    EXPECT_EQ(entries.at(1).count, 2U);
    EXPECT_FALSE(entries.at(0).thumb);
}

TEST(ExecutedAddresses, KeepsThumbAndArmApart) {
    memory::PagedMemory memory;
    Load(memory,
         {ThumbMove & 0xFF,
          ThumbMove >> 8,
          ThumbAdd & 0xFF,
          ThumbAdd >> 8,
          ThumbAdd & 0xFF,
          ThumbAdd >> 8});
    CpuState state;
    state.registers.at(ProgramCounter) = Code;
    state.thumb = true;

    ExecutedAddresses executed;
    std::uint64_t done = 0;
    static_cast<void>(arm::Run(state, memory, 3, done, executed));

    EXPECT_EQ(executed.CountOf(Code, true), 1U);
    EXPECT_EQ(executed.CountOf(Code + 2, true), 1U);
    EXPECT_EQ(executed.CountOf(Code + 4, true), 1U);
    EXPECT_EQ(executed.CountOf(Code, false), 0U);
    for (const ExecutedAddresses::Entry& entry : executed.Entries()) {
        EXPECT_TRUE(entry.thumb);
    }
}

TEST(ExecutedAddresses, SeesTheInstructionThatFaultsToo) {
    memory::PagedMemory memory;
    CpuState state;
    state.registers.at(ProgramCounter) = Code;

    ExecutedAddresses executed;
    std::uint64_t done = 0;
    const Stop stop = arm::Run(state, memory, 5, done, executed);

    EXPECT_EQ(stop.reason, StopReason::FetchFault);
    EXPECT_EQ(done, 0U);
    EXPECT_EQ(executed.CountOf(Code, false), 1U) << "told before the fetch, as documented";
}

TEST(ExecutedAddresses, ObservingDoesNotChangeWhatTheRunComputes) {
    memory::PagedMemory plain_memory;
    memory::PagedMemory observed_memory;
    const std::vector<std::uint8_t> program = ArmBytes({MovZero, AddOne, BranchBack});
    Load(plain_memory, program);
    Load(observed_memory, program);
    CpuState plain;
    CpuState observed_state;
    plain.registers.at(ProgramCounter) = Code;
    observed_state.registers.at(ProgramCounter) = Code;

    ExecutedAddresses executed;
    std::uint64_t plain_done = 0;
    std::uint64_t observed_done = 0;
    static_cast<void>(arm::Run(plain, plain_memory, 1000, plain_done));
    static_cast<void>(arm::Run(observed_state, observed_memory, 1000, observed_done, executed));

    EXPECT_EQ(plain_done, observed_done);
    EXPECT_EQ(plain.registers, observed_state.registers);
}

}  // namespace
}  // namespace hexlith::domain::arm
