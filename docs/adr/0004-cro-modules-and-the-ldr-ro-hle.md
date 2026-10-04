# 0004. CRO modules are lifted position-independently

Status: Accepted. Provisional from 2026-10-03, settled on 2026-10-04 by the trace described under "What settled it".
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


The instrumented build this asked for turned out to be unnecessary: Azahar already logs each load with the module name and the resulting address, at a level a release build compiles in. [`docs/ldr-ro-trace.md`](../ldr-ro-trace.md) has the procedure and what each answer means.

## What settled it

One run under Azahar, from a fresh boot through the introduction, the choice of starter and the first battle, with field transitions on the way. [`docs/ldr-ro-trace.md`](../ldr-ro-trace.md) has the full account.

The bases are not deterministic, and the run did not need a second run to show it. Four modules were loaded at more than one address within that one run: `DllBackGround` at `0x006C8000`, `0x006C4000` and `0x007BB000`, `DllSequence` at `0x006D1000` and `0x006A5000`, `DllStrInput` at `0x006B5000` and `0x006A5000`, and `DllReport` at `0x007E9000` and `0x007E5000`. Modules are unloaded and reloaded constantly, 31 loads and 30 unloads in seven minutes, and whatever is loaded first after a screen change takes the lowest address, `0x006A5000`.

The reason is in the service's interface rather than in the trace. The caller of `LoadCRO` passes the address the module is to be mapped at, and the buffers for its data and BSS, in the request. Source: 3dbrew, `RO:LoadExeCRO`, request words 2, 4 and 7. So a module lands wherever the game's own allocator put it, which depends on what was loaded before it and in what order.

Fixed-base lifting is therefore ruled out, and position-independent lifting is the only correct option, as the decision above already assumed.

The optimisation this record described, turning cross-module calls into direct host calls, does not depend on the addresses after all. When the loader links an import it knows which export of which module the import resolves to, and so which lifted function it is. It can fill the module's thunk table with that host function at link time, whatever address the module was given. That is what the loader will do; there is no fixed address left to assert.

## Consequences

The `ldr:ro` reimplementation and the CRO parser have to exist early, before lifting is useful, which moves them forward in the schedule.

The reimplementation maps a module at the address the game asks for, rather than choosing one, and has to support unloading and loading again at a different address any number of times.

The CRO parser lives in `src/domain/ctr` and is used by both the offline extractor and the runtime loader, for the reason given in [ADR 0001](0001-language-and-build-system.md).

Generated code for a module cannot contain absolute guest addresses from that module. That constrains the lifter's output in a way the main executable's does not, so the lifter has to handle both cases rather than assuming one.

The export and import tables are also a symbol source. `static.crs` names 461 symbols in the main executable, and each module names its own exports. See [ADR 0006](0006-analysis-layer-format.md).
