# 0002. The lifter emits C++ rather than LLVM IR

Status: Accepted
Date: 2026-10-03

## Context

The lifter translates ARM machine code into something the host toolchain can compile. The measured scale is 8,782,284 bytes of executable ARM, roughly 2.2 million instructions, split between the main executable and 133 dynamically loaded modules.

Two project requirements bear directly on this. Generated code has to be readable by a human, within reason. A function has to be replaceable by a hand-written one when the translation is wrong or too slow.

## Options

**Emit C++.** Each guest function becomes a host function. The host compiler does the optimisation. This is what TriAevum and the Nintendo 64 recompilation projects do.

**Emit LLVM IR.** Lift to LLVM's intermediate representation and let LLVM generate objects directly.

**Hybrid.** Emit C++ by default, with an LLVM path for the few shards where it measurably pays.

## Decision

Emit C++.

The lifter keeps its own typed, serialisable intermediate representation so the hybrid option stays open without rewriting the front end.

## Why not LLVM

The usual argument for LLVM is optimisation quality, and most of that advantage is one specific thing: eliminating dead computation of the processor's condition flags.

ARM updates N, Z, C and V on most arithmetic. Translated literally, every instruction emits several lines computing flags that nothing subsequently reads. Removing them is a liveness analysis: for each flag write, look forward and ask whether anything reads it before it is overwritten. That analysis is well understood and Hexlith can run it on its own representation, which captures the bulk of the benefit without the dependency.

Against that, LLVM IR is not readable and cannot be hand-patched, which conflicts with both requirements above. It also adds a large build dependency and ties the project to a specific LLVM version at exactly the layer that should stay stable longest.

## Consequences

Compile time becomes a first-order design constraint rather than an afterthought. Millions of generated lines will not build in one translation unit. The output is sharded across many files so compilation parallelises and only changed shards rebuild, ccache is wired into the build from the start, and link-time optimisation has to be the incremental kind rather than one monolithic pass.

This is why [ADR 0001](0001-language-and-build-system.md) puts ccache in the build rather than leaving it to each developer. If the edit, build and test cycle reaches hours, development stops regardless of how good the generated code is.

Hexlith owns the flag liveness analysis and its correctness. A bug there produces wrong arithmetic in code that looks right, which is why [ADR 0003](0003-interpreter-as-the-semantic-reference.md) exists.

The host compiler's own optimiser does the register allocation and local work, which it is good at, so the lifter does not need to.
