# 0003. An ARM interpreter is the semantic reference

Status: Accepted
Date: 2026-10-03

## Context

The point of this project is to stop interpreting code. Building an interpreter looks like work in the wrong direction.

The ARM11 is an ARMv6K core with VFPv2 floating point. Getting its behaviour right means getting condition flags, saturating arithmetic, the shifter, long multiplies, exclusive access and the floating point control register all right, including rounding mode, flush-to-zero and NaN handling.

## Options

**No interpreter.** Lift everything and test the result against the game running.

**Interpreter as a development tool**, discarded once the lifter works.

**Interpreter as the permanent semantic reference**, shipped in diagnostic builds.

## Decision

Build the interpreter first, and keep it as the reference.

The interpreter and the generated code call the same primitive operation library. When the lifter emits an add that sets flags, it emits a call to the same function the interpreter calls.

## Why

Four independent reasons, any one of which would be sufficient.

The interpreter is an executable specification. Given the same input, the interpreter and the lifted code must produce the same registers, flags and memory. When they disagree, one of them is wrong and that is known immediately rather than inferred from a graphical glitch. This is the basis of differential testing against Azahar: run the same code in both, compare the processor state step by step, and the first divergence is the bug.

Some branch targets are computed at runtime. When a computed target lands on an address that was never lifted, something has to execute it.

Before the lifter produces anything runnable there is nothing to debug with. The interpreter is what makes the first boot attempt possible, and the first boot attempt is what reveals which services are actually needed.

Reading generated C++ tells you what was generated. Single-stepping an interpreter tells you what the original code does, which is a different and often more useful question.

## The shared primitive library

This is the part that matters. If the interpreter has its own addition with flags and the lifter emits its own, the two will differ in some corner, and the differential test will be comparing two bugs.

A single library, in `src/domain/arm`, implements each operation once. The interpreter calls it per instruction. Generated code calls it inline, where the host compiler can specialise and the flag liveness analysis can elide calls whose results nothing reads.

Divergence then requires the lifter to emit the wrong call rather than a subtly different implementation, which is a far easier class of mistake to catch.

## Consequences

Two execution paths exist permanently, which is more code to maintain. The interpreter is always built in CI and can be excluded from release builds by a CMake option.

The interpreter must be fast enough to be useful, but it does not have to be fast enough to play the game. Running a few thousand instructions in a test, or a scene in a trace comparison, is the requirement.

Writing the interpreter first means the ARM semantics get settled before any of them are baked into generated code, which is the right order. Discovering a flag bug after lifting 2.2 million instructions would mean regenerating all of them.
