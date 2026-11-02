#include "application/lift_verification.h"

#include <algorithm>
#include <format>

namespace hexlith::application {
namespace {

namespace kernel = domain::kernel;
namespace arm = domain::arm;

using Address = std::uint64_t;
constexpr Address AddressSpaceSize = Address{1} << 32U;
constexpr std::uint32_t PageSize = domain::memory::ProcessMemory::PageSize;

std::optional<std::string> RegisterDifference(const arm::CpuState& interpreted,
                                              const arm::CpuState& lifted) {
    for (std::size_t index = 0; index < interpreted.registers.size(); ++index) {
        if (interpreted.registers.at(index) != lifted.registers.at(index)) {
            return std::format("r{} is {:08X} interpreted and {:08X} lifted",
                               index,
                               interpreted.registers.at(index),
                               lifted.registers.at(index));
        }
    }
    for (std::size_t index = 0; index < interpreted.float_registers.size(); ++index) {
        if (interpreted.float_registers.at(index) != lifted.float_registers.at(index)) {
            return std::format("s{} is {:08X} interpreted and {:08X} lifted",
                               index,
                               interpreted.float_registers.at(index),
                               lifted.float_registers.at(index));
        }
    }
    const auto flags = [](const arm::CpuState& state) {
        return std::format("N{:d} Z{:d} C{:d} V{:d} Q{:d} GE{:X} T{:d} FPSCR {:08X}",
                           state.negative,
                           state.zero,
                           state.carry,
                           state.overflow,
                           state.saturated,
                           state.greater_or_equal,
                           state.thumb,
                           state.fpscr);
    };
    if (flags(interpreted) != flags(lifted)) {
        return std::format(
            "the flags are {} interpreted and {} lifted", flags(interpreted), flags(lifted));
    }
    const auto monitor = [](const arm::CpuState& state) {
        return std::format("{:d} {:08X} {:08X} {:08X}",
                           state.exclusive_open,
                           state.exclusive_address,
                           state.thread_id_read_write,
                           state.thread_id_read_only);
    };
    if (monitor(interpreted) != monitor(lifted)) {
        return std::format("the exclusive monitor or thread IDs are {} interpreted and {} lifted",
                           monitor(interpreted),
                           monitor(lifted));
    }
    return std::nullopt;
}

std::optional<std::string> MemoryDifference(const domain::memory::ProcessMemory& interpreted,
                                            const domain::memory::ProcessMemory& lifted) {
    for (Address page = 0; page < AddressSpaceSize; page += PageSize) {
        const auto address = static_cast<std::uint32_t>(page);
        const std::span<const std::uint8_t> left = interpreted.PageBytes(address);
        const std::span<const std::uint8_t> right = lifted.PageBytes(address);
        if (left.empty() != right.empty()) {
            return std::format("the page at {:08X} is mapped in only one run", address);
        }
        const auto [first, second] = std::ranges::mismatch(left, right);
        if (first != left.end()) {
            const auto offset = static_cast<std::uint32_t>(first - left.begin());
            return std::format("the byte at {:08X} is {:02X} interpreted and {:02X} lifted",
                               address + offset,
                               *first,
                               *second);
        }
    }
    return std::nullopt;
}

// Runs a kernel on until it has executed a number of instructions in all, a
// step at a time, so that two runs brought to the same count were stopped at
// the same places on the way.
kernel::KernelStop Advance(kernel::Kernel& running, std::uint64_t target, std::uint64_t step) {
    kernel::KernelStop stop;
    while (running.Executed() < target) {
        const std::uint64_t done = running.Executed();
        const std::uint64_t next = std::min(target, ((done / step) + 1) * step);
        stop = running.Run(next - done);
        if (stop.reason != kernel::KernelStopReason::BudgetSpent) {
            break;
        }
    }
    return stop;
}

// Both runs started again and brought to the same count, with what differs.
struct Pair {
    std::unique_ptr<VerifiedRun> interpreted;
    std::unique_ptr<VerifiedRun> lifted;
};

core::Result<Pair> Start(const VerifiedRunMaker& make) {
    core::Result<std::unique_ptr<VerifiedRun>> interpreted = make(false);
    if (!interpreted) {
        return interpreted.Failure();
    }
    core::Result<std::unique_ptr<VerifiedRun>> lifted = make(true);
    if (!lifted) {
        return lifted.Failure();
    }
    return Pair{.interpreted = std::move(interpreted.Value()), .lifted = std::move(lifted.Value())};
}

// What differs once both runs have reached a count, and why the
// interpreter's stopped if it stopped before it.
struct Comparison {
    std::optional<std::string> difference;
    kernel::KernelStop stop;
};

Comparison AfterBoth(Pair& pair, std::uint64_t target, std::uint64_t step) {
    const kernel::KernelStop left = Advance(pair.interpreted->Kernel(), target, step);
    const kernel::KernelStop right = Advance(pair.lifted->Kernel(), target, step);
    if (left.reason != right.reason || left.detail != right.detail) {
        return {.difference = std::format("the runs stopped for different reasons, {} and {}",
                                          static_cast<int>(left.reason),
                                          static_cast<int>(right.reason)),
                .stop = left};
    }
    return {.difference = DifferenceBetween(pair.interpreted->Kernel(), pair.lifted->Kernel()),
            .stop = left};
}

// The thread a difference names was where the interpreter left it at the last
// count both agreed on; the first thread when the difference is in memory.
std::uint32_t ProgramCounterAt(const kernel::Kernel& running) {
    const auto& threads = running.Threads().Threads();
    return threads.empty() ? 0 : threads.front()->context.registers.at(arm::ProgramCounter);
}

}  // namespace

std::optional<std::string> DifferenceBetween(const kernel::Kernel& interpreted,
                                             const kernel::Kernel& lifted) {
    if (interpreted.Executed() != lifted.Executed()) {
        return std::format("{} instructions were executed interpreted and {} lifted",
                           interpreted.Executed(),
                           lifted.Executed());
    }
    const auto& left = interpreted.Threads().Threads();
    const auto& right = lifted.Threads().Threads();
    if (left.size() != right.size()) {
        return std::format(
            "there are {} threads interpreted and {} lifted", left.size(), right.size());
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::optional<std::string> differs =
                RegisterDifference(left.at(index)->context, right.at(index)->context)) {
            return std::format("thread {}: {}", left.at(index)->id, *differs);
        }
    }
    return MemoryDifference(interpreted.Space().Memory(), lifted.Space().Memory());
}

core::Result<LiftVerification>
VerifyLiftedCode(const VerifiedRunMaker& make, std::uint64_t limit, std::uint64_t step) {
    core::Result<Pair> started = Start(make);
    if (!started) {
        return started.Failure();
    }
    Pair& pair = started.Value();
    LiftVerification verification;
    std::uint64_t agreed = 0;
    std::uint32_t pc = ProgramCounterAt(pair.interpreted->Kernel());
    std::optional<std::string> differs;
    while (agreed < limit) {
        const std::uint64_t target = std::min(limit, agreed + step);
        const Comparison compared = AfterBoth(pair, target, step);
        differs = compared.difference;
        if (differs) {
            break;
        }
        const std::uint64_t reached = pair.interpreted->Kernel().Executed();
        if (reached < target) {
            // Both stopped alike before the step was over.
            verification.agreed = reached;
            verification.stop = compared.stop;
            return verification;
        }
        agreed = target;
        pc = ProgramCounterAt(pair.interpreted->Kernel());
    }
    verification.agreed = agreed;
    if (!differs) {
        return verification;
    }

    // Somewhere after agreed and no later than the end of the step. The
    // difference kept is always the one found at the count kept as different.
    std::uint64_t different = std::min(limit, agreed + step);
    while (different - agreed > 1) {
        const std::uint64_t middle = agreed + ((different - agreed) / 2);
        core::Result<Pair> again = Start(make);
        if (!again) {
            return again.Failure();
        }
        const Comparison compared = AfterBoth(again.Value(), middle, step);
        if (compared.difference) {
            different = middle;
            differs = compared.difference;
        } else {
            agreed = middle;
            pc = ProgramCounterAt(again.Value().interpreted->Kernel());
        }
    }
    verification.agreed = agreed;
    verification.divergence =
        LiftDivergence{.instructions = different, .difference = *differs, .last_agreed_pc = pc};
    return verification;
}

}  // namespace hexlith::application
