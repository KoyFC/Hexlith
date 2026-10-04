#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/memory/paged_memory.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <vector>

namespace hexlith::domain::arm {
namespace {

// Every encoding here was checked against Capstone before it was written down,
// and carries its disassembly in a comment. These pin the behaviour a reader can
// check by eye; scripts/compare_arm_interpreter.py checks the rest against
// Unicorn.

constexpr std::uint32_t Code = 0x00100000;
constexpr std::uint32_t Data = 0x00200000;
constexpr std::uint32_t Stack = 0x00300000;
constexpr std::uint32_t PageSize = memory::PagedMemory::PageSize;

// A small address space: a read-only page of code, a page of data and a page of
// stack, with the stack pointer at the top of its page.
class Machine {
public:
    Machine() {
        EXPECT_TRUE(memory.Map(Code, PageSize, false));
        EXPECT_TRUE(memory.Map(Data, PageSize, true));
        EXPECT_TRUE(memory.Map(Stack, PageSize, true));
        state.registers.at(ProgramCounter) = Code;
        state.registers.at(StackPointer) = Stack + PageSize;
    }

    void Arm(std::initializer_list<std::uint32_t> words) {
        std::vector<std::uint8_t> bytes;
        for (const std::uint32_t word : words) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        EXPECT_TRUE(memory.Load(Code, bytes));
    }

    void Thumb(std::initializer_list<std::uint16_t> halfwords) {
        std::vector<std::uint8_t> bytes;
        for (const std::uint16_t halfword : halfwords) {
            bytes.push_back(static_cast<std::uint8_t>(halfword));
            bytes.push_back(static_cast<std::uint8_t>(halfword >> 8U));
        }
        EXPECT_TRUE(memory.Load(Code, bytes));
        state.thumb = true;
    }

    Stop Step() {
        return arm::Step(state, memory);
    }

    Stop Run(std::uint64_t limit) {
        return arm::Run(state, memory, limit);
    }

    std::uint32_t& R(RegisterIndex index) {
        return state.registers.at(index);
    }

    std::uint32_t Pc() const {
        return state.registers.at(ProgramCounter);
    }

    CpuState state;
    memory::PagedMemory memory;
};

TEST(Interpreter, AddsAndSetsTheFlags) {
    Machine machine;
    // mov r0, #1; adds r0, r0, #1
    machine.Arm({0xE3A00001, 0xE2900001});
    EXPECT_EQ(machine.Run(2).reason, StopReason::StepLimit);
    EXPECT_EQ(machine.R(0), 2U);
    EXPECT_FALSE(machine.state.zero);
    EXPECT_EQ(machine.Pc(), Code + 8);
}

TEST(Interpreter, SkipsAnInstructionWhoseConditionFails) {
    Machine machine;
    // mov r0, #1; cmp r0, #2; moveq r1, #5; movne r1, #6
    machine.Arm({0xE3A00001, 0xE3500002, 0x03A01005, 0x13A01006});
    machine.Run(4);
    EXPECT_EQ(machine.R(1), 6U);
}

TEST(Interpreter, CallsAndReturns) {
    Machine machine;
    // bl 0x100008; svc #0x12; bx lr
    machine.Arm({0xEB000000, 0xEF000012, 0xE12FFF1E});
    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_EQ(machine.Pc(), Code + 8);
    EXPECT_EQ(machine.R(LinkRegister), Code + 4);

    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_EQ(machine.Pc(), Code + 4);

    const Stop call = machine.Step();
    EXPECT_EQ(call.reason, StopReason::SupervisorCall);
    EXPECT_EQ(call.detail, 0x12U);
    EXPECT_EQ(machine.Pc(), Code + 8) << "the kernel returns past the call";
}

TEST(Interpreter, ExchangesIntoThumbAndBack) {
    Machine machine;
    // blx 0x100008, which lands in Thumb two words on; then bx lr in Thumb.
    machine.Arm({0xFA000000, 0, 0x00004770});
    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_TRUE(machine.state.thumb);
    EXPECT_EQ(machine.Pc(), Code + 8);
    EXPECT_EQ(machine.R(LinkRegister), Code + 4) << "an ARM return address has no Thumb bit";

    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_FALSE(machine.state.thumb);
    EXPECT_EQ(machine.Pc(), Code + 4);
}

TEST(Interpreter, LoadsALiteralAndTransfersWithWriteBack) {
    Machine machine;
    // ldr r0, [pc]; str r0, [r1, #4]!; and the literal, two words after the
    // load, where the pipeline offset puts it.
    machine.Arm({0xE59F0000, 0xE5A10004, 0xCAFEF00D});
    machine.R(1) = Data;
    machine.Run(2);
    EXPECT_EQ(machine.R(0), 0xCAFEF00DU);
    EXPECT_EQ(machine.R(1), Data + 4) << "pre-indexed with write-back";
    EXPECT_EQ(machine.memory.Read32(Data + 4), 0xCAFEF00DU);
}

TEST(Interpreter, PostIndexesAndExtends) {
    Machine machine;
    // ldr r0, [r1], #4; ldrh r0, [r1]; ldrsb r0, [r1]
    machine.Arm({0xE4910004, 0xE1D100B0, 0xE1D100D0});
    ASSERT_TRUE(machine.memory.Write32(Data, 0x11111111));
    ASSERT_TRUE(machine.memory.Write32(Data + 4, 0x0000F080));
    machine.R(1) = Data;

    machine.Step();
    EXPECT_EQ(machine.R(0), 0x11111111U);
    EXPECT_EQ(machine.R(1), Data + 4) << "post-indexed always writes back";
    machine.Step();
    EXPECT_EQ(machine.R(0), 0xF080U);
    machine.Step();
    EXPECT_EQ(machine.R(0), 0xFFFFFF80U);
}

TEST(Interpreter, LoadsAnUnalignedWord) {
    Machine machine;
    // ldr r0, [r1]
    machine.Arm({0xE5910000});
    ASSERT_TRUE(machine.memory.Write32(Data, 0x44332211));
    ASSERT_TRUE(machine.memory.Write32(Data + 4, 0x88776655));
    machine.R(1) = Data + 1;
    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_EQ(machine.R(0), 0x55443322U) << "unaligned, not rotated";
}

TEST(Interpreter, PushesAndPopsIntoTheProgramCounter) {
    Machine machine;
    // push {r4, lr}; pop {r4, pc}
    machine.Arm({0xE92D4010, 0xE8BD8010});
    machine.R(4) = 0x44;
    machine.R(LinkRegister) = Data + 1;

    machine.Step();
    EXPECT_EQ(machine.R(StackPointer), Stack + PageSize - 8);
    EXPECT_EQ(machine.memory.Read32(Stack + PageSize - 8), 0x44U);

    machine.R(4) = 0;
    machine.Step();
    EXPECT_EQ(machine.R(4), 0x44U);
    EXPECT_EQ(machine.Pc(), Data) << "a loaded program counter interworks";
    EXPECT_TRUE(machine.state.thumb);
}

TEST(Interpreter, FaultsOnAnUnalignedDoubleword) {
    Machine machine;
    // ldrd r2, r3, [r1]
    machine.Arm({0xE1C120D0});
    machine.R(1) = Data + 2;
    const Stop stop = machine.Step();
    EXPECT_EQ(stop.reason, StopReason::AlignmentFault);
    EXPECT_EQ(stop.detail, Data + 2);
    EXPECT_EQ(machine.Pc(), Code) << "a stop leaves the instruction to be retried";
}

TEST(Interpreter, RefusesALoadMultipleThatWritesBackIntoItsOwnList) {
    Machine machine;
    // ldm r1!, {r0, r1}
    machine.Arm({0xE8B10003});
    machine.R(1) = Data;
    EXPECT_EQ(machine.Step().reason, StopReason::Unpredictable);
}

TEST(Interpreter, LeavesTheStateAloneOnAReadFault) {
    Machine machine;
    // ldr r0, [r1], #4, from an unmapped address
    machine.Arm({0xE4910004});
    machine.R(0) = 7;
    machine.R(1) = 0x00900000;
    const Stop stop = machine.Step();
    EXPECT_EQ(stop.reason, StopReason::ReadFault);
    EXPECT_EQ(stop.detail, 0x00900000U);
    EXPECT_EQ(machine.R(0), 7U);
    EXPECT_EQ(machine.R(1), 0x00900000U) << "no write-back either";
}

TEST(Interpreter, StoresExclusivelyOnlyWhileTheMonitorIsOpen) {
    Machine machine;
    // ldrex r0, [r1]; strex r2, r0, [r1]; strex r2, r0, [r1]
    machine.Arm({0xE1910F9F, 0xE1812F90, 0xE1812F90});
    machine.R(1) = Data;
    machine.Run(2);
    EXPECT_EQ(machine.R(2), 0U) << "the first store succeeds";
    machine.Step();
    EXPECT_EQ(machine.R(2), 1U) << "the monitor closed after the first one";
}

TEST(Interpreter, StopsWhereItCannotGoOn) {
    Machine breakpoint;
    // bkpt #0
    breakpoint.Arm({0xE1200070});
    EXPECT_EQ(breakpoint.Step().reason, StopReason::Breakpoint);

    Machine undefined;
    // udf #0
    undefined.Arm({0xE7F000F0});
    const Stop stop = undefined.Step();
    EXPECT_EQ(stop.reason, StopReason::UndefinedInstruction);
    EXPECT_EQ(stop.detail, 0xE7F000F0U);

    Machine fetch;
    fetch.state.registers.at(ProgramCounter) = 0x00900000;
    EXPECT_EQ(fetch.Step().reason, StopReason::FetchFault);
}

TEST(Interpreter, ReadsAndWritesTheStatusRegister) {
    Machine machine;
    // msr apsr_nzcvq, #0xF0000000; mrs r0, apsr
    machine.Arm({0xE328F20F, 0xE10F0000});
    machine.Run(2);
    EXPECT_TRUE(machine.state.negative && machine.state.zero && machine.state.carry &&
                machine.state.overflow);
    EXPECT_EQ(machine.R(0), 0xF0000010U) << "the flags, and user mode";
}

TEST(Interpreter, MultipliesHalvesAndSaturates) {
    Machine machine;
    // smulbb r0, r1, r2
    machine.Arm({0xE1600281});
    machine.R(1) = 0x0000FFFE;
    machine.R(2) = 0x00000003;
    machine.Step();
    EXPECT_EQ(machine.R(0), static_cast<std::uint32_t>(-6));

    Machine saturating;
    // qadd r0, r2, r1
    saturating.Arm({0xE1010052});
    saturating.R(1) = 1;
    saturating.R(2) = 0x7FFFFFFF;
    saturating.Step();
    EXPECT_EQ(saturating.R(0), 0x7FFFFFFFU);
    EXPECT_TRUE(saturating.state.saturated);
}

TEST(Interpreter, SelectsByTheFlagsAParallelAdditionLeft) {
    Machine machine;
    // uadd8 r0, r1, r2; sel r0, r0, r1
    machine.Arm({0xE6510F92, 0xE6800FB1});
    machine.R(1) = 0x010203FF;
    machine.R(2) = 0x01010101;
    machine.Step();
    EXPECT_EQ(machine.R(0), 0x02030400U);
    EXPECT_EQ(machine.state.greater_or_equal, 0b0001);
    machine.Step();
    EXPECT_EQ(machine.R(0), 0x01020300U) << "byte 0 from the sum, the rest from r1";
}

TEST(Interpreter, ReadsTheThreadPointer) {
    Machine machine;
    // mrc p15, 0, r0, c13, c0, 3
    machine.Arm({0xEE1D0F70});
    machine.state.thread_id_read_only = 0x1FF82000;
    EXPECT_FALSE(machine.Step().Stopped());
    EXPECT_EQ(machine.R(0), 0x1FF82000U);
}

TEST(Interpreter, RunsThumb) {
    Machine machine;
    // movs r0, #1; adds r0, r0, r1; muls r0, r1, r0
    machine.Thumb({0x2001, 0x1840, 0x4348});
    machine.R(1) = 3;
    machine.Run(3);
    EXPECT_EQ(machine.R(0), 12U);
    EXPECT_EQ(machine.Pc(), Code + 6);
}

TEST(Interpreter, AlignsTheProgramCounterForThumbAddresses) {
    Machine machine;
    // movs r0, #1; ldr r0, [pc, #4]; adr r1, #4; then the literal. The load at
    // Code + 2 reads from Align(Code + 2 + 4, 4) + 4.
    machine.Thumb({0x2001, 0x4801, 0xA101, 0x0000, 0xBEEF, 0xDEAD});
    machine.Run(3);
    EXPECT_EQ(machine.R(0), 0xDEADBEEFU);
    EXPECT_EQ(machine.R(1), Code + 4 + 4 + 4) << "adr at Code + 4 reads Code + 8, plus 4";
}

TEST(Interpreter, CallsInThumbWithAFourByteBranch) {
    Machine machine;
    // bl 0x100004; svc #5
    machine.Thumb({0xF000, 0xF800, 0xDF05});
    machine.Step();
    EXPECT_EQ(machine.Pc(), Code + 4);
    EXPECT_EQ(machine.R(LinkRegister), (Code + 4) | 1U) << "a Thumb return address is odd";
    EXPECT_EQ(machine.Step().reason, StopReason::SupervisorCall);
}

TEST(Interpreter, PopsIntoThumbCode) {
    Machine machine;
    // push {r4, lr}; pop {r4, pc}
    machine.Thumb({0xB510, 0xBD10});
    machine.R(LinkRegister) = (Code + 0x40) | 1U;
    machine.Run(2);
    EXPECT_EQ(machine.Pc(), Code + 0x40);
    EXPECT_TRUE(machine.state.thumb);
}

}  // namespace
}  // namespace hexlith::domain::arm
