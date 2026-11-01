# 0024. The lifter lowers instruction by instruction, and the interpreter takes what it does not

Status: Accepted
Date: 2026-11-01

## Context

[ADR 0002](0002-lifter-emits-cpp-not-llvm-ir.md) has the lifter keep its own typed intermediate representation and [ADR 0003](0003-interpreter-as-the-semantic-reference.md) makes the interpreter the reference the lifted code is checked against. Neither says what the representation looks like, and both leave a risk standing: the lifter has to restate what each of some 120 operations does, and every restatement is a place to differ from the interpreter by one corner.

The interpreter's executors are written against an `Execution` object that holds the instruction as data, so they cannot be reused as they are. Compiling each instruction as a call to them with the instruction as a constant would reuse the logic exactly, and it is what I looked at first. It makes unreadable generated code, which ADR 0002 rules out, and it asks the host compiler to fold a large generic function away a million times.

## Decision

**An instruction becomes a short list of operations, and the list is the unit.** Each operation reads or writes a named piece of processor state, or computes on temporaries that live and die inside the instruction, or is a call to one of the shared primitives. There is no graph and no single static assignment: the host compiler does that, and what has to survive between instructions is processor state, which the operations name.

**Whatever the lowering does not translate becomes one `Interpret` operation.** The interpreter executes that instruction whole, from the state it is in, including its condition. This is exact by construction, and it is what lets the lowering grow one class of instruction at a time, each class checked on its own, with the rest of the program already running. Instructions the architecture calls unpredictable go the same way, decided from the decoded instruction by the rule `primitives::HasUnpredictableOperands` that the interpreter uses.

**What cannot be known until the code runs is a bail.** A load the memory refuses, an address that has to be aligned, a target that is only halfword aligned: the lifted code leaves for the interpreter at that instruction, with the state as it was before it, and the interpreter re-executes it and says why it stops. That is exact only if no operation of the instruction has changed anything before the check, so the lowering orders every check before the first write. The operations of the instructions it translates are idempotent under re-execution where they cannot be ordered that way, and the block's clock charge for the instructions that did not run is given back.

**An evaluator gives the IR its reference meaning.** `Evaluate` runs one instruction's operations against a processor state and a memory, calling the same primitives. The lowering is then checked without building any C++, by running an instruction both ways from the same state and comparing, which the tests do for every class of instruction over every combination of the condition flags. The generated code is a second translation of the same operations and is checked on its own.

## Why not the alternatives

**The executors, specialised by the compiler.** Exact, and unreadable, and slow to compile, as above.

**Static single assignment form.** It would let the lifter do its own register allocation, which the host compiler does better.

**Everything at once.** Lowering all of the instruction set before running any of it means the first thing to find out how the pieces fit is a bug in the last of them.

## Consequences

The lowering is the one piece of new logic that restates what an instruction means, and it is the thing the differential tests exist for. Each class of instruction it learns to translate comes with the check that it agrees with the interpreter, and an instruction it has not learned costs speed and nothing else.

The first classes translated are data processing with its shifter, multiplies, single loads and stores with every addressing mode, the branches, the hints and the thread ID registers. Block transfers, the long multiplies, saturating and media instructions, status register access and floating point are still the interpreter's, and the survey of the real image decides which to translate next.
