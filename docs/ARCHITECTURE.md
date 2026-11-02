# Architecture

How Hexlith is put together, and why. Decisions with lasting consequences are recorded separately in [`adr/`](adr/); this document is the map that makes those decisions legible.

Most of what follows exists. A cartridge is unpacked and analysed, the main executable is lifted to C++ and runs as a library under a reimplemented kernel and its services, and the screens are drawn with Vulkan. The CRO modules are still interpreted, and no sound is made. Each section marks what is present and what is not, because a design document that reads as though the system already works is worse than no document at all.

## What is being built

A native PC executable that runs Pokémon X by translating its code ahead of time rather than interpreting it at runtime.

The pipeline runs locally, on the user's own cartridge image, and never redistributes anything derived from it:

```
cartridge image
      |  extract      validate against the GameProfile, unpack ExeFS, RomFS, ExHeader, CRO modules
      v
  workspace
      |  analyse      find function boundaries, classify code and data, resolve symbols
      v
  analysis artefacts
      |  lift         emit readable C++ per function
      v
  generated C++
      |  build        compile into a shared library in the workspace
      v
lifted library, loaded and run by hexlith-boot
```

All four stages exist for the main executable. [`extraction.md`](extraction.md) describes extraction, [`classification.md`](classification.md) and [`analysis-tables.md`](analysis-tables.md) describe analysis, and [`lifter.md`](lifter.md) describes lifting and building, with [ADR 0026](adr/0026-lifted-code-is-built-in-the-workspace-and-loaded-as-a-library.md) for why the result is a library. Analysis covers the main executable and each of the 133 CRO modules, which are analysed in address spaces of their own and seeded from their relocation tables. Lifting the modules is Phase 9, and [ADR 0029](adr/0029-cro-modules-are-lifted-against-a-base-the-loader-gives.md) is its starting design.

The repository holds the tools and the runtime. The workspace, which holds the cartridge image and everything derived from it, lives outside the repository and is never redistributed. See [ADR 0007](adr/0007-no-game-data-in-the-repository.md) and [ADR 0008](adr/0008-workspace-configuration.md).

## Layers

Dependencies point inward. A layer uses those below it and never one above.

```
tools  ->  adapters  ->  application  ->  ports  ->  domain  ->  core
```

**`src/core`** holds small utilities: a `Result` type, bounds-checked byte reading, SHA-256, UTF-16 transcoding for RomFS names, and the `key = value` text the workspace configuration is written in. Nothing here knows what a 3DS is. Its one external dependency is OpenSSL's libcrypto, linked privately by the SHA-256 wrapper so that it reaches nothing else. See [ADR 0009](adr/0009-sha256-from-openssl.md).

**`src/domain`** holds pure logic and no I/O at all. The ARM processor model, the guest memory map, kernel abstractions, the CTR container and CRO formats, the analysis types, the `GameProfile`. Everything here is testable against synthetic data, which is what makes the test suite able to run without a cartridge.

**`src/application`** holds use cases. Extract a title, validate an image against a profile, analyse a code region, lift a function. A use case orchestrates domain types and reaches the outside world only through a port.

**`src/ports`** holds the interfaces through which the outside world is reached: `RomSource`, `BlobSink` and `TreeStore` for files, `InputSource` for the player, `ScreenSink` and `GpuBackend` for the screens, and `GpuRecorder` and `GpuRecordingSource` for a recording of the GPU's work. Sound has none yet, since none is made. The first two are narrow on purpose: a `RomSource` can only be read, and a `BlobSink` owns the directory its relative paths resolve against, so a use case never sees an absolute path and cannot construct one.

**`src/adapters`** holds the implementations: the filesystem, Vulkan, SDL, and the loader of the library of lifted code. Vulkan and SDL are libraries of their own, so that only what draws links Vulkan and glslang and only what opens a window links SDL.

**`tools`** holds the command-line programs. A tool wires adapters to a use case and does nothing else, so that the interesting code is never trapped inside a `main`.

### Why this shape

Two reasons, both practical rather than doctrinal.

The analysis and lifting logic is the part most likely to be wrong, and it is the part that is cheapest to test if it has no I/O. Putting the format knowledge and the ARM semantics in a layer that cannot open a file forces that.

The second reason is that the same container and CRO parsers are needed twice: once offline by the extractor, and again at runtime by the `ldr:ro` reimplementation when the game loads a module. One implementation in `domain` serves both. Two implementations would drift, and the drift would appear as a wrong address in generated code.

### Where the abstraction stops

Abstraction belongs at system boundaries. It does not belong in a loop that runs millions of times a second.

Lifted code, guest memory access and GPU command processing avoid per-instruction virtual dispatch and use static polymorphism with flat data structures instead. Every place where that rule overrides the layering is documented where it is made, with the measurement or the reasoning that justified it.

The intent is that a `GpuBackend` is a virtual interface chosen once at startup, while the function that reads a guest word is not virtual at all.

## The runtime

Everything here exists in a first form except sound. What each piece still lacks is said with it.

**Guest memory** is a page table: one host pointer per 4 KiB guest page, into backings that stand for the console's physical memory, so that two guest addresses can share bytes the way Horizon's aliases require. A guest word read is an index and a host load. [ADR 0012](adr/0012-guest-memory-is-a-page-table.md) records why it is not the flat allocation first planned, and [`horizon-memory.md`](horizon-memory.md) describes the address space the kernel builds in it. Memory-mapped I/O, misaligned access and exclusive access stay explicit transactions.

**The CPU** has two implementations of the same semantics. Lifted C++ is the fast path. An ARM interpreter is the reference, the fallback for addresses that were never lifted, and the oracle for differential testing. Both call the same primitive library so they cannot diverge. See [ADR 0003](adr/0003-interpreter-as-the-semantic-reference.md).

The lifter, in `src/domain/lift`, turns the main executable into C++ that `hexlith-boot --lifted` runs, leaving to the interpreter what it does not lower, and `hexlith-lift-verify` runs both side by side to the first instruction they disagree on. [ADRs 0024](adr/0024-the-lifter-lowers-instruction-by-instruction-and-the-interpreter-takes-the-rest.md) to [0026](adr/0026-lifted-code-is-built-in-the-workspace-and-loaded-as-a-library.md) record how, and [`lifter.md`](lifter.md) what it measured. The modules' code is still interpreted.

The interpreter and the primitive library exist, in `src/domain/arm/interpreter` and `src/domain/arm/primitives`, and run ARM, Thumb and VFPv2 floating point in user mode. They are checked against Unicorn as [`arm-interpreter.md`](arm-interpreter.md) describes. Floating point runs on the host's own arithmetic with the edges handled explicitly, which [ADR 0010](adr/0010-floating-point-on-the-host.md) records. The interpreter runs against anything that satisfies the `GuestMemory` concept, and is compiled for two: the page table above, and a sparse paged memory for tests and tools.

**The kernel** is reimplemented at a high level. The game's system calls land in host functions rather than in emulated kernel code. Threads, events, mutexes, semaphores, timers and address arbiters are modelled, because a scheduler that is merely plausible produces deadlocks rather than crashes. All of the game's threads share one host thread under a strict priority scheduler, and time is counted in instructions executed, so that a run is the same every time; [ADR 0013](adr/0013-one-host-thread-and-a-virtual-clock.md) records why, and [`horizon-kernel.md`](horizon-kernel.md) what is modelled.

**Services** implement a common interface and live in a registry, keyed by the port name the game connects to. The extended header's service access control list names every service the title can reach, which makes it the work list, and srv: refuses any other. A request is answered on the calling thread within its SendSyncRequest, and a command nothing implements stops the run by name. See [`ipc.md`](ipc.md) and [`formats/exheader.md`](formats/exheader.md).

**Dynamic modules** are the hard part. Roughly half the title's executable code sits in 133 CRO modules that are relocated at load time. [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) records the approach, and the trace that showed a module can land at a different address each time it is loaded. The linking exists in `src/domain/linker`: given the modules and the addresses chosen for them, it computes what every relocated word holds, the way the service does. The `ldr:ro` service is built on it and leaves a module in memory as the console's loader does. [`cro-linking.md`](cro-linking.md) describes both. Lifting the modules is Phase 9, and [ADR 0029](adr/0029-cro-modules-are-lifted-against-a-base-the-loader-gives.md) is its starting design.

**Graphics** translate PICA200 state and command lists into Vulkan. Azahar's `video_core` is the reference for PICA behaviour and `nihstro` for its shader format.

The PICA state is its register file ([ADR 0017](adr/0017-pica-state-is-the-register-file.md)). A shader interpreter ([ADR 0018](adr/0018-a-shader-interpreter-is-the-semantic-reference.md)) and a reference rasterizer that follows Azahar's software renderer ([ADR 0019](adr/0019-the-reference-rasterizer-follows-azahar-and-samples-two-ways.md)) are the semantic reference. The Vulkan backend translates the vertex shaders to GLSL ([ADR 0020](adr/0020-pica-shaders-become-glsl-compiled-by-glslang.md)), keeps GPU memory true ([ADR 0022](adr/0022-the-vulkan-backend-keeps-gpu-memory-true.md)) and is held to the reference within a measured tolerance ([ADR 0021](adr/0021-the-vulkan-backend-is-held-to-the-reference-within-a-measured-tolerance.md)). `hexlith-boot --window` draws the screens as the title runs ([ADRs 0027](adr/0027-the-screens-are-drawn-as-the-game-runs-and-shown-in-a-window.md) and [0028](adr/0028-the-live-display-draws-in-the-titles-own-memory.md)). [`pica-commands.md`](pica-commands.md), [`pica-shaders.md`](pica-shaders.md), [`pica-rasterizer.md`](pica-rasterizer.md) and [`gpu-reference.md`](gpu-reference.md) say what each was checked against.

**Audio** reimplements the DSP in software. Azahar's `audio_core/hle` is the reference. It does not exist yet: the `dsp::DSP` service answers what the boot asks of it, from the firmware load to an audio frame at the console's rate, and mixes nothing. [`ipc.md`](ipc.md) has what it answers.

## Game-specific knowledge

Core logic never asks which game is running. It asks the `GameProfile`.

The profile holds the identity of a title and version, its segment addresses, its content hashes, its module list and its symbol tables. Adding Pokémon Y later means writing a profile, not editing the lifter. See [ADR 0005](adr/0005-game-profile-design.md).

Only Pokémon X, program ID `0004000000055D00`, base version, is supported or testable. No other title is claimed.

## Analysis artefacts

Lifting needs to know where functions begin and end, which bytes are code and which are data, and what to call things. That knowledge accumulates over the life of the project, some of it derived automatically and some of it entered by hand, so it is stored as text that a person can edit and that regenerates without reordering. Every record carries where it came from. See [ADR 0006](adr/0006-analysis-layer-format.md).

The starting point is the 461 named exports in `static.crs`, which is the only symbol information the cartridge gives away.

`hexlith-analyze` writes the four tables beside the extraction in the workspace, not into the repository. It walks the main executable from its known entry points with the ARM and Thumb decoders in `src/domain/arm`, which are checked against Capstone as [`arm-decoder.md`](arm-decoder.md) describes.

## Formats

Every container and module format Hexlith reads is documented in [`formats/`](formats/), with a source for each field and an explicit note wherever the documentation and the data disagree. Nothing is invented.

## Prior art

[TriAevum](https://github.com/coccofresco/TriAevum) is a native recompilation of Ocarina of Time 3D and the closest existing work. [`triaevum-audit.md`](triaevum-audit.md) records what Hexlith takes from it and what it leaves behind.

The short version is that its container extraction is directly reusable, its Azahar-derived graphics and audio code is a good donor for later phases, and almost everything else is either Ocarina-specific or built on N64 infrastructure that does not apply. It has no CRO support at all, so the hardest part of this project has no precedent there.
