# Architecture

How Hexlith is put together, and why. Decisions with lasting consequences are recorded separately in [`adr/`](adr/); this document is the map that makes those decisions legible.

Most of what follows does not exist yet. Phase 0 built the guardrails and wrote this down. Each section marks what is present and what is planned, because a design document that reads as though the system already works is worse than no document at all.

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
      |  build        compile and link against the Hexlith runtime
      v
native executable
```

Only the first stage exists. It is Phase 1.

The repository holds the tools and the runtime. The workspace, which holds the cartridge image and everything derived from it, lives outside the repository and is never redistributed. See [ADR 0007](adr/0007-no-game-data-in-the-repository.md) and [ADR 0008](adr/0008-workspace-configuration.md).

## Layers

Dependencies point inward. A layer uses the one below it and never the one above.

```
tools  ->  adapters  ->  ports  ->  application  ->  domain  ->  core
```

**`src/core`** holds small utilities: a `Result` type, bounds-checked byte reading, SHA-256, deterministic text output. Nothing here knows what a 3DS is. Its one external dependency is OpenSSL's libcrypto, linked privately by the SHA-256 wrapper so that it reaches nothing else. See [ADR 0009](adr/0009-sha256-from-openssl.md).

**`src/domain`** holds pure logic and no I/O at all. The ARM processor model, the guest memory map, kernel abstractions, the CTR container and CRO formats, the analysis types, the `GameProfile`. Everything here is testable against synthetic data, which is what makes the test suite able to run without a cartridge.

**`src/application`** holds use cases. Extract a title, validate an image against a profile, analyse a code region, lift a function. A use case orchestrates domain types and reaches the outside world only through a port.

**`src/ports`** holds the interfaces through which the outside world is reached: `RomSource`, `BlobSink`, `GpuBackend`, `AudioSink`, `InputSource`.

**`src/adapters`** holds the implementations: the filesystem, Vulkan, SDL.

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

Planned, not built. Recorded here so the pieces have names.

**Guest memory** is a flat host allocation with the 3DS memory map laid out inside it. Contiguous RAM ranges get validated host views so that a guest word read compiles to a host load rather than a function call. Memory-mapped I/O, misaligned access and exclusive access stay explicit transactions.

**The CPU** has two implementations of the same semantics. Lifted C++ is the fast path. An ARM interpreter is the reference, the fallback for addresses that were never lifted, and the oracle for differential testing. Both call the same primitive library so they cannot diverge. See [ADR 0003](adr/0003-interpreter-as-the-semantic-reference.md).

**The kernel** is reimplemented at a high level. The game's system calls land in host functions rather than in emulated kernel code. Threads, events, mutexes and address arbiters are modelled, because a scheduler that is merely plausible produces deadlocks rather than crashes.

**Services** implement a common interface and live in a registry, keyed by the port name the game connects to. The extended header's service access control list names every service the title can reach, which makes it the work list. See [`formats/exheader.md`](formats/exheader.md).

**Dynamic modules** are the hard part. Roughly half the title's executable code sits in 133 CRO modules that are relocated at load time. [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) records the approach and why the decision is still provisional.

**Graphics** translate PICA200 state and command lists into Vulkan. Azahar's `video_core` is the reference for PICA behaviour and `nihstro` for its shader format.

**Audio** reimplements the DSP in software. Azahar's `audio_core/hle` is the reference.

## Game-specific knowledge

Core logic never asks which game is running. It asks the `GameProfile`.

The profile holds the identity of a title and version, its segment addresses, its content hashes, its module list and its symbol tables. Adding Pokémon Y later means writing a profile, not editing the lifter. See [ADR 0005](adr/0005-game-profile-design.md).

Only Pokémon X, program ID `0004000000055D00`, base version, is supported or testable. No other title is claimed.

## Analysis artefacts

Lifting needs to know where functions begin and end, which bytes are code and which are data, and what to call things. That knowledge accumulates over the life of the project, some of it derived automatically and some of it entered by hand, so it is stored as text that a person can edit and that regenerates without reordering. Every record carries where it came from. See [ADR 0006](adr/0006-analysis-layer-format.md).

The starting point is the 461 named exports in `static.crs`, which is the only symbol information the cartridge gives away.

## Formats

Every container and module format Hexlith reads is documented in [`formats/`](formats/), with a source for each field and an explicit note wherever the documentation and the data disagree. Nothing is invented.

## Prior art

[TriAevum](https://github.com/coccofresco/TriAevum) is a native recompilation of Ocarina of Time 3D and the closest existing work. [`triaevum-audit.md`](triaevum-audit.md) records what Hexlith takes from it and what it leaves behind.

The short version is that its container extraction is directly reusable, its Azahar-derived graphics and audio code is a good donor for later phases, and almost everything else is either Ocarina-specific or built on N64 infrastructure that does not apply. It has no CRO support at all, so the hardest part of this project has no precedent there.
