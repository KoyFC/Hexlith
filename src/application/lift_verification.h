#ifndef HEXLITH_APPLICATION_LIFT_VERIFICATION_H
#define HEXLITH_APPLICATION_LIFT_VERIFICATION_H

#include "core/result.h"
#include "domain/kernel/kernel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace hexlith::application {

// One of the two runs the verifier drives side by side: a kernel, and
// whatever has to stay alive for it to run, such as a booted title or a lifted
// library.
class VerifiedRun {
public:
    VerifiedRun() = default;
    virtual ~VerifiedRun() = default;

    VerifiedRun(const VerifiedRun&) = delete;
    VerifiedRun& operator=(const VerifiedRun&) = delete;
    VerifiedRun(VerifiedRun&&) = delete;
    VerifiedRun& operator=(VerifiedRun&&) = delete;

    [[nodiscard]] virtual domain::kernel::Kernel& Kernel() = 0;
};

// Starts a run from the beginning, through the interpreter alone or through
// lifted code. Two runs it makes have to be the same run but for that, which
// is what a new game with a script of input is.
using VerifiedRunMaker = std::function<core::Result<std::unique_ptr<VerifiedRun>>(bool lifted)>;

// The first point at which lifted code did not do what the interpreter did:
// how many instructions both runs had executed, and what differed.
struct LiftDivergence {
    std::uint64_t instructions = 0;
    std::string difference;
    // Where the thread that differs was, in the interpreter's run, one step
    // before: the last point at which the two agreed.
    std::uint32_t last_agreed_pc = 0;
};

struct LiftVerification {
    // Instructions after which the two runs were found alike.
    std::uint64_t agreed = 0;
    std::optional<LiftDivergence> divergence;
    // Why the interpreter's run stopped, when both stopped alike before the
    // limit.
    std::optional<domain::kernel::KernelStop> stop;
};

// Runs the interpreter and lifted code side by side, a step of instructions
// at a time, and compares the two after each step: every thread's registers
// and flags, and every mapped page of memory. Lifted code is meant to leave
// exactly what the interpreter leaves after any number of instructions
// (ADR 0025), so the first step after which they differ holds a mistake.
//
// That step is then narrowed to the instruction by starting both runs again
// and stopping them at the middle of what is left, as many times as it takes.
// Lifted code runs a block only when the run has the whole block's
// instructions left, so the instruction found is the last of the block that
// went wrong.
core::Result<LiftVerification>
VerifyLiftedCode(const VerifiedRunMaker& make, std::uint64_t limit, std::uint64_t step);

// What differs between two kernels' threads and memory, or nothing.
std::optional<std::string> DifferenceBetween(const domain::kernel::Kernel& interpreted,
                                             const domain::kernel::Kernel& lifted);

}  // namespace hexlith::application

#endif  // HEXLITH_APPLICATION_LIFT_VERIFICATION_H
