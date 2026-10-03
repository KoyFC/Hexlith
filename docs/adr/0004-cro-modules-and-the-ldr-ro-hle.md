# 0004. CRO modules are lifted position-independently

Status: Provisional. Settled by the condition named below.
Date: 2026-10-03

## Context

Pokémon X loads roughly half its executable code from dynamically linked modules. Measured from the reference image:

| | Bytes |
| --- | --- |
| Code segments across 133 CRO modules | 4,342,512 |
| Text segment of the main executable | 4,439,772 |

That is 49.4 percent of the title's code in modules that the `ldr:ro` service maps, relocates and links at runtime. Across the 134 CRO-family files there are 44,151 internal relocations, 21,868 external relocations and 18,991 anonymous imports. `DllField.cro` alone carries 10,237 internal relocations and exports two symbols.

A CRO does not know where it will be loaded. The service places it, then walks its relocation tables patching every address inside it. Cross-module calls resolve against whichever modules are loaded at the time.

This is the single largest difference from the prior art. Ocarina of Time 3D is one fixed-address image, so TriAevum has no CRO handling and nothing here can be borrowed from it.

Module names indicate per-mode loading: `DllField`, `DllBattle`, `DllBox`, `DllZukan` and so on correspond to game screens, not to libraries that would stay resident. Whether the game keeps them all mapped or loads and unloads them has not been measured.

## Options

**Fixed-base lifting.** Determine the load address of each module once, record it in the GameProfile, and lift each module with its addresses baked into the generated C++. The `ldr:ro` reimplementation then asserts that each module lands where the profile says.

Fast, because a cross-module call becomes a direct host call. Correct only if the addresses really are deterministic across every path through the game.

**Position-independent lifting.** Lift each module without knowing its address. References that the original resolved by absolute address become lookups through a per-module table that the loader fills in. Cross-module calls go through an import thunk.

Correct regardless of load order. Costs one indirection on cross-module calls and more complexity in the lifter.

**Interpret the modules, lift only the main executable.** Leaves 49.4 percent of the code interpreted, which defeats the purpose.

## Decision

Lift position-independently. Resolve references through per-module tables filled in by the `ldr:ro` reimplementation at load time.

Then, as an optimisation and only once it is verified: when the loader confirms the expected module set at the expected addresses, patch the thunk table so that cross-module calls become direct host calls. This recovers the speed of fixed-base lifting without depending on the assumption being true.

## Why not fixed-base first

Because of how it fails. A wrong load address does not crash. It produces a call to a different function that happens to be at that address, and the consequence surfaces somewhere else entirely, possibly hours into a playthrough, possibly only in one scene. Debugging that means suspecting the address map, which is the one thing that looked settled.

The evidence that the assumption is safe does not exist yet. Module names suggest per-mode loading and unloading, 133 modules total 7.65 MB against a linear heap that could hold them, and nothing has been measured either way. Choosing the fast option on an unmeasured assumption, in the place where a wrong assumption is hardest to detect, is the wrong trade for a project whose first goal is correctness.

## What settles this

An instrumented run of the title under Azahar, recording every `ldr:ro` load and unload with the module name and the resulting base address, across a boot, a field transition and a battle.

If the bases are deterministic, the optimisation is enabled with the loader asserting the expected address rather than trusting it. If they are not, the position-independent path is the only correct option and the question was worth asking.

This is Phase 2 work. Until then the lifter is written against the position-independent model, which is the one that is correct either way.

## Consequences

The `ldr:ro` reimplementation and the CRO parser have to exist early, before lifting is useful, which moves them forward in the schedule.

The CRO parser lives in `src/domain/ctr` and is used by both the offline extractor and the runtime loader, for the reason given in [ADR 0001](0001-language-and-build-system.md).

Generated code for a module cannot contain absolute guest addresses from that module. That constrains the lifter's output in a way the main executable's does not, so the lifter has to handle both cases rather than assuming one.

The export and import tables are also a symbol source. `static.crs` names 461 symbols in the main executable, and each module names its own exports. See [ADR 0006](0006-analysis-layer-format.md).
