# 0025. Lifted code answers where the guest continues, and can be entered at any block

Status: Accepted
Date: 2026-11-02

## Context

Lifted functions are host functions that call each other, and the guest's threads are switched by a kernel that decides when, at system calls and at timed events ([ADR 0013](0013-one-host-thread-and-a-virtual-clock.md)). A host function cannot be suspended in the middle and resumed on another thread, and the clock has to agree with the interpreter's to the instruction, so that a slice can end between any two instructions. Both facts are about leaving lifted code and coming back to it.

## Decision

**A lifted function runs the guest from an address until the guest leaves it, and answers the address the guest continues at.** A caller checks the answer against the address after its call. If they match, the callee returned and the caller goes on. If not, the caller passes the answer up, and at the top it reaches the engine, which continues from there. The guest's whole state is in the `CpuState` and in guest memory wherever code can leave, so passing an answer up is always safe.

Two answers are not addresses. An address the guest can continue at has its low bit clear, since both instruction sets align to at least two bytes, so odd numbers are free: 1 says the lifted code cannot go on at `cpu.pc` and the interpreter should execute an instruction, and 3 says the interpreter stopped and the context says why.

**Every function can be entered at the start of any of its blocks.** The function begins with a switch on the address it is given that jumps to the block's label. The engine therefore resumes a thread natively wherever the kernel left it: after a system call, after a call that returned to a frame that is gone, in the middle of a loop. The alternative, entering only at function entries and interpreting the rest of the function, would make a loop that waits on a system call run interpreted for ever.

**The clock is charged by block, and given back by whoever leaves early.** Entering a block costs its instruction count. A block whose cost exceeds what the slice has left leaves for the interpreter before running anything, and the interpreter finishes the slice an instruction at a time. A bail or a stop hands back the instructions of the block that did not run, the stopping one included unless it completed, as a supervisor call does. The engine then counts exactly what `arm::Run` counts, which the tests check by stopping after every number of instructions from one to sixty-nine.

**Calls are host calls and returns are host returns.** A direct call is a call of the lifted function; an indirect one looks the target up in a table of block starts and calls what it finds, or answers the target when there is nothing. An indirect jump is the same lookup as a tail call. A switch is a `goto` between blocks of the function, because going through the lookup for every iteration of a loop would recurse.

## Why not the alternatives

**A fibre for each guest thread.** It would keep native frames across a system call. It cannot replace what is described above, because a slice can end in the middle of any block, which needs the code to be resumable at an arbitrary instruction boundary anyway, and a platform specific stack switch is a lot to add before that is measured.

**Unwinding with exceptions.** Leaving is the common path for a thread that waits every frame, and an answer in a register is cheaper and simpler to read than a throw.

**Only function entries in the table.** Rejected above.

## Consequences

Registers and flags live in the `CpuState` at the start of this work, which is what makes leaving safe at any block boundary. Holding them in host registers inside a function is an optimisation that has to write them back at every point where code can leave, and is a later decision, taken with a measurement.

The entry table is a hash map from address to function for each instruction set. It is the first thing to replace with a flat array if it shows up in a profile.

The engine has to run the interpreter between bits of lifted code, and does it by looking up every address. That is acceptable while most of the code is lifted and is the first place to look if it is not.
