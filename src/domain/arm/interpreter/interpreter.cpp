#include "domain/arm/interpreter/interpreter.h"

#include "domain/arm/interpreter/step.h"
#include "domain/memory/paged_memory.h"
#include "domain/memory/process_memory.h"

namespace hexlith::domain::arm {

std::string_view Describe(StopReason reason) {
    switch (reason) {
    case StopReason::None:
        return "none";
    case StopReason::StepLimit:
        return "step limit";
    case StopReason::SupervisorCall:
        return "supervisor call";
    case StopReason::Breakpoint:
        return "breakpoint";
    case StopReason::UndefinedInstruction:
        return "undefined instruction";
    case StopReason::Unpredictable:
        return "unpredictable";
    case StopReason::Privileged:
        return "privileged";
    case StopReason::Unsupported:
        return "unsupported";
    case StopReason::FetchFault:
        return "fetch fault";
    case StopReason::ReadFault:
        return "read fault";
    case StopReason::WriteFault:
        return "write fault";
    case StopReason::AlignmentFault:
        return "alignment fault";
    }
    return "unknown";
}

template Stop Step<memory::PagedMemory>(CpuState&, memory::PagedMemory&);
template Stop Run<memory::PagedMemory>(CpuState&, memory::PagedMemory&, std::uint64_t);
template Stop Step<memory::ProcessMemory>(CpuState&, memory::ProcessMemory&);
template Stop Run<memory::ProcessMemory>(CpuState&, memory::ProcessMemory&, std::uint64_t);
template Stop
Run<memory::PagedMemory>(CpuState&, memory::PagedMemory&, std::uint64_t, std::uint64_t&);
template Stop
Run<memory::ProcessMemory>(CpuState&, memory::ProcessMemory&, std::uint64_t, std::uint64_t&);

}  // namespace hexlith::domain::arm
