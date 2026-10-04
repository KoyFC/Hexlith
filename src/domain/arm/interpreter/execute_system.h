#ifndef HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_SYSTEM_H
#define HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_SYSTEM_H

#include "domain/arm/instruction.h"
#include "domain/arm/interpreter/execution.h"

#include <cstdint>

// The instructions that talk to the system rather than compute: the supervisor
// call, the breakpoint, the hints, the byte order, and the system control
// coprocessor.
//
// Internal to the interpreter.
namespace hexlith::domain::arm::interpreter {

// CP15, the system control coprocessor. User mode reaches three things in it on
// the ARM11, and everything else is privileged:
//
// - the thread ID registers in c13, c0, opcode 2 read-write and opcode 3
//   read-only, the second of which Horizon uses to point each thread at its
//   local storage;
// - the barriers in c7: c10 4 drains the write buffer, c10 5 is the data memory
//   barrier and c5 4 flushes the prefetch buffer, which ARMv7 renamed DSB, DMB
//   and ISB.
//
// Source: the ARM11 MPCore technical reference manual, the c7 and c13 register
// descriptions. A single thread in an interpreter has nothing to order, so the
// barriers complete without doing anything.
namespace cp15 {
constexpr std::uint8_t Coprocessor = 15;
constexpr RegisterIndex CacheOperations = 7;
constexpr RegisterIndex ThreadIds = 13;
constexpr std::uint8_t ReadWriteThreadId = 2;
constexpr std::uint8_t ReadOnlyThreadId = 3;
}  // namespace cp15

template<class Memory>
bool IsUserBarrier(const Execution<Memory>& execution) {
    constexpr RegisterIndex WriteBuffer = 10;
    constexpr RegisterIndex PrefetchBuffer = 5;
    constexpr std::uint8_t Drain = 4;
    constexpr std::uint8_t MemoryBarrier = 5;
    constexpr std::uint8_t Flush = 4;

    const Instruction& instruction = execution.instruction;
    if (instruction.operation != Operation::MoveToCoprocessor ||
        instruction.first != cp15::CacheOperations || instruction.coprocessor_opcode1 != 0) {
        return false;
    }
    const RegisterIndex crm = instruction.second;
    const std::uint8_t opcode = instruction.coprocessor_opcode2;
    return (crm == WriteBuffer && (opcode == Drain || opcode == MemoryBarrier)) ||
           (crm == PrefetchBuffer && opcode == Flush);
}

template<class Memory>
Stop ExecuteCoprocessor(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;
    if (instruction.coprocessor != cp15::Coprocessor) {
        return Halt(execution, StopReason::Unsupported);
    }
    if (IsUserBarrier(execution)) {
        return Completed();
    }

    const bool thread_id = instruction.first == cp15::ThreadIds && instruction.second == 0 &&
                           instruction.coprocessor_opcode1 == 0;
    if (!thread_id) {
        return Halt(execution, StopReason::Privileged);
    }
    if (instruction.destination == ProgramCounter) {
        return Halt(execution, StopReason::Unpredictable);
    }

    std::uint32_t& rt = execution.state->registers.at(instruction.destination);
    switch (instruction.coprocessor_opcode2) {
    case cp15::ReadWriteThreadId:
        if (instruction.operation == Operation::MoveFromCoprocessor) {
            rt = execution.state->thread_id_read_write;
        } else {
            execution.state->thread_id_read_write = rt;
        }
        return Completed();
    case cp15::ReadOnlyThreadId:
        if (instruction.operation == Operation::MoveFromCoprocessor) {
            rt = execution.state->thread_id_read_only;
            return Completed();
        }
        return Halt(execution, StopReason::Privileged);
    default:
        break;
    }
    return Halt(execution, StopReason::Privileged);
}

template<class Memory>
Stop ExecuteSystem(Execution<Memory>& execution) {
    const Instruction& instruction = execution.instruction;

    switch (instruction.operation) {
    case Operation::SoftwareInterrupt:
        // The kernel returns to the instruction after the call, so the program
        // counter moves on before the interpreter hands over.
        execution.state->registers.at(ProgramCounter) = execution.Next();
        execution.branched = true;
        return Halt(StopReason::SupervisorCall, instruction.immediate);
    case Operation::Breakpoint:
        return Halt(StopReason::Breakpoint, instruction.immediate);

    // Hints. A yield, a wait for an event and an event are scheduling advice
    // that a single thread can ignore, and a wait for interrupt may complete at
    // once: the architecture allows every one of them to behave as a no
    // operation.
    case Operation::NoOperation:
    case Operation::Yield:
    case Operation::WaitForEvent:
    case Operation::WaitForInterrupt:
    case Operation::SendEvent:
    case Operation::Preload:
        return Completed();

    // Little-endian is a no operation. Big-endian data is a mode nothing in the
    // project supports yet.
    case Operation::SetEndianness:
        return instruction.immediate == 0 ? Completed() : Halt(execution, StopReason::Unsupported);

    case Operation::ChangeProcessorState:
    case Operation::StoreReturnState:
    case Operation::ReturnFromException:
    case Operation::SecureMonitorCall:
        return Halt(execution, StopReason::Privileged);

    case Operation::MoveToCoprocessor:
    case Operation::MoveFromCoprocessor:
        return ExecuteCoprocessor(execution);
    case Operation::CoprocessorDataOperation:
    case Operation::MoveToCoprocessorPair:
    case Operation::MoveFromCoprocessorPair:
    case Operation::LoadCoprocessor:
    case Operation::StoreCoprocessor:
        return Halt(execution, StopReason::Unsupported);
    default:
        break;
    }
    return Halt(execution, StopReason::UndefinedInstruction);
}

}  // namespace hexlith::domain::arm::interpreter

#endif  // HEXLITH_DOMAIN_ARM_INTERPRETER_EXECUTE_SYSTEM_H
