# 0029. CRO modules are lifted against a base the loader gives

Status: Provisional, for Phase 9 to build. Settled by the measurement under "What settles this".
Date: 2026-11-02

## Context

Phase 8 lifted the main executable and nothing else. Past the first frames of a boot that is the smaller part of what runs: a boot to frame 1101 executes 3.87 billion instructions, 66 percent of them in the 133 CRO modules, all interpreted, and with lifted code it takes 171 seconds where the boot to frame 227, almost all main executable, takes a third of a second of the game's code.

[ADR 0004](0004-cro-modules-and-the-ldr-ro-hle.md) settled that modules are lifted position-independently: the game passes the address a module is mapped at to `LoadCRO`, and one run loaded the same module at three different addresses. The main executable's lifted code is written for fixed addresses. Every branch target, every address a block is entered at and every address handed back to the engine is a constant.

What is already in place for modules: the analysis reads each one at its file offsets, with the module as if mapped at zero, and reaches 94.6 percent of the module code with 43,805 functions and no conflicts. The loader, `services::RelocatableObjects`, says when a module is loaded or unloaded and where. Lifted code already treats every word in a literal pool as something to load rather than a constant, because `ldr:ro` rewrites some of them ([ADR 0026](0026-lifted-code-is-built-in-the-workspace-and-loaded-as-a-library.md)).

## Decision

**A module's code is lifted at offsets from its own start, and runs against a base held in the context.** The emitter writes every address a module's code computes as `base + offset`: branch targets, return addresses, the address an exit hands back, the address a bail or the interpreter is given. The switch a function is entered through is on `pc - base`. The base is a field of `runtime::Context`, set by the engine before it enters a module's function and unchanged by a direct branch or call inside the module, which cannot leave it. Code that reads a literal pool reads the word from guest memory, where `ldr:ro` has relocated it, as the main executable's code already does.

**All the modules go into one library of their own,** `libhexlith_modules.so`, built like the main executable's (ADR 0026) and apart from it, so that a change to one does not rebuild the other. Its manifest names the lifter's version and the SHA-256 of every module it was lifted from, and a module whose digest does not match when it is loaded runs interpreted, with a warning, rather than through code lifted from other bytes.

**The engine finds a module's code through the modules loaded now.** The library registers one table of entries per module, by offset. When the loader reports a module loaded, the engine records the range of its code segment, its base and its table; when it reports one unloaded, it forgets them. An address the main executable's table cannot cover is looked up in the loaded ranges, and the function found is entered with that module's base. A module loaded twice at once would be two ranges with one table.

**A call from one module to another, or to the main executable, stays an indirect call** through the pointer the loader wrote, and goes through the engine's lookup. ADR 0004's optimisation, binding an import to the lifted function it resolves to when the loader links it, is left for after the rest works and is checked.

**Runs record a module's executed instructions by module and offset,** a second form of `observed/executed.txt`, so that the analysis can take them as facts as it does for the main executable ([`classification.md`](../classification.md#what-a-run-executed-as-a-fact)). An absolute address means nothing across runs once modules move.

**`hexlith-lift-verify` checks it as it is.** It compares whole machines, so it does not need to know where a module is.

## Why not the alternatives

**One library per module, opened when the game loads the module and closed when it unloads it.** It is the shape the game itself has, and it makes 133 libraries to build and track, a `dlopen` on every screen change, and code that must not be running when it is closed. One library opened once costs nothing at a load.

**Relocate host code at load time,** lifting at base zero and patching the constants in the host's machine code when the module is placed. It needs a relocation format for the generated code of its own, and the compiler would have to be kept from folding the constants it is told to leave alone. An addition per address is cheaper to get right.

**Lift each module once for every address it has been seen at.** The addresses depend on what was loaded before, so the list is never complete, and code for an address not seen would silently be missing.

## What settles this

That no relocation in any of the 133 modules writes a word the analysis classifies as an instruction. Lifted code takes instructions from the file and literals from memory, so a relocated instruction would make the lifted code differ from what the processor runs, and nothing else in this design would notice. The relocation tables are read already (`hexlith-cro-dump`) and so are the regions, so this is a count to make before the first module is lifted. If the count is not zero, the modules that have such a word are lifted with that instruction left to the interpreter, which reads it from memory.

## Consequences

Every address a module's code computes costs an addition, and entering a module's function costs a range lookup when the main executable's table does not cover the address. Both are small beside an instruction run by the interpreter, which is what every module instruction is now.

The engine starts to depend on the loader's events, which until now only the golden trace and the stop points listened to. The loader already keeps a list of observers, so the engine is one more.

The table of executed instructions changes form, and `scripts/executed_coverage.py` has to read the new one.
