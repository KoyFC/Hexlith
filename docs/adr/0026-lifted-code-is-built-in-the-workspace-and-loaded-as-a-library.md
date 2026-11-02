# 0026. Lifted code is built in the workspace and loaded as a shared library

Status: Accepted
Date: 2026-11-02

## Context

The lifted C++ is derived from the game, so it can never enter the repository ([ADR 0007](0007-no-game-data-in-the-repository.md)). It is also large. For the main executable, 22,489 functions come to 273 source files, 11.7 million lines and 469 MB of text, and building it is the longest step in the project: about eight minutes on eight cores for a cold build, nearly an hour of processor time. Whatever the structure, the repository's own build must stay quick, offline and free of anything derived from the game.

## Decision

**`hexlith-lift --emit` writes a small CMake project into the workspace**, beside the extraction: a header with every function's declaration, a source file per shard of about four thousand instructions, a registry, and a `CMakeLists.txt`. `scripts/build-lifted.sh` builds it from there, with ccache when it is installed, into `libhexlith_lifted.so`.

**The binaries of the repository load the library by path.** `hexlith-boot --lifted <path>` opens it with `dlopen`, reads a manifest function, and refuses it unless the manifest names this version of the lifter and the SHA-256 of the code the title was loaded with. That check is the only protection against lifted code run against other code, which does not fail loudly. The library then fills the table of entries.

**The library resolves what it calls from the binary that loads it.** The generated code calls the interpreter, guest memory and the primitives that are not inline, and does not link them in: `hexlith-boot` is linked with its symbols exported. An undefined symbol is then a failure to open the library, with the name of the symbol.

**Shards are the unit of compilation.** A change to the lifter that alters the output of a few functions changes a few shard files, and with ccache only those are compiled again. Each shard is compiled at `-O1`, with `-ffp-contract=off` for the reason in [ADR 0010](0010-floating-point-on-the-host.md) and with warnings off, since the code is written by a program.

## Why not the alternatives

**Link the lifted code into the binary.** The binary would then contain the game, and building the repository would mean building it.

**One translation unit per function, or one for everything.** 22,489 object files is a lot of process start-up, and one file is a build that cannot be parallelised or resumed.

**Generate at run time.** A compiler at start-up is a minute of delay on every run, for code that does not change between runs.

## Consequences

The first run of lifted code found something that changed a decision of the lifter. The main executable's import stubs jump through a word that `ldr:ro` rewrites when it loads a module, so the stubs cannot be read as branches to a constant. The assumption that the executable's text does not change was wrong for those literals, and the lifter now treats every jump through a literal as an indirect jump.

A library is tied to the layout of the structures it shares with the binary, the `CpuState` and the memory, which the manifest's version number stands for. Changing either means regenerating and rebuilding the library, which the version bump forces.
