# Hexlith

A static recompilation of Pokémon X for the PC.

The goal is to run the game natively rather than emulate it: translate its ARM11 code to C++ ahead of time, reimplement the Horizon kernel and its services at a high level, translate PICA200 graphics to Vulkan, and emulate the DSP in software.

## Status

Early. Nothing is playable yet. The game boots to its title screen and can be shown in a window as it runs, its main executable as lifted C++ and its modules interpreted, with no sound.

What works is extraction. `hexlith-extract` validates a cartridge image against a game profile and writes its contents into a working directory: the extended header, the ExeFS with its executable decompressed, the RomFS as a directory tree, and a manifest of every file with its hash. Two runs produce identical manifests. [`docs/extraction.md`](docs/extraction.md) describes it.

Analysis works too. `hexlith-analyze` reads an extraction and works out which bytes are instructions, which encoding each one is in, and where each function begins and ends, writing four line-oriented tables that a person can edit and that a later run will not overwrite. It does the same for each of the 133 CRO modules, in an address space of its own and seeded from the module's relocation tables. On the reference cartridge it reaches 80.3 percent of the main executable's text segment and 94.6 percent of the modules' code, and finds 17,309 and 43,805 functions, in under two seconds. [`docs/classification.md`](docs/classification.md) describes it and says what it does not reach.

Under it is a complete ARMv6K, Thumb and VFPv2 decoder, checked against an independent disassembler over five million random words and over the whole executable, which agrees on every instruction and on every branch target. [`docs/arm-decoder.md`](docs/arm-decoder.md) records what that comparison caught.

On top of the decoder is an interpreter for ARM, Thumb and VFPv2 floating point, the executable reference the lifted code is held to. It shares one library of primitive operations with the code the lifter emits, and it agrees with Unicorn's ARM11 model on every one of 811,391 random and real instructions it was compared on. [`docs/arm-interpreter.md`](docs/arm-interpreter.md) describes it and what the comparison caught.

The CRO modules can be linked offline the way the console's `ldr:ro` service links them: given the modules and the addresses chosen for them, Hexlith works out what every relocated word holds. Its reading of the import tables agrees with every count Azahar logged over three runs, and the words it computes agree with the memory of a module Azahar had loaded, on all 2,880 that were compared. [`docs/cro-linking.md`](docs/cro-linking.md) describes it.

The game boots to its title screen. `hexlith-boot` runs it from its entry point on the interpreter, under a reimplemented kernel with a deterministic scheduler and the services its boot reaches, with the player's input read from a script of frames. A new game formats its save data, goes through the language selection on two presses of A, unloads that module and loads the title screen's three, 42 million instructions and 227 frames in about two seconds. Each module, header and tables included, and the buffer the unload gives back match Azahar's memory at the same instant byte for byte, and two runs give the same instructions and the same golden trace. The run can write a golden trace to compare with Azahar's and a recording of the GPU's work for drawing later. [`docs/ipc.md`](docs/ipc.md), [`docs/cro-linking.md`](docs/cro-linking.md) and [`docs/golden-traces.md`](docs/golden-traces.md) describe it.

The screens are drawn. A reference renderer that, sampling textures as Azahar does, agrees with Azahar's software renderer on every step of a recording, and a Vulkan backend held to it within a measured tolerance, draw what the title asks for, and `hexlith-boot --window` shows both screens as the title runs, with the player at the keyboard and mouse. [`docs/gpu-reference.md`](docs/gpu-reference.md) describes how the two are held to each other.

The main executable is lifted. `hexlith-lift` writes its functions out as C++, over 22,000 of them, which is built into a library in the workspace and run by `hexlith-boot --lifted`, with the interpreter taking the instructions the lifter does not translate. The boot to the title screen executes the same 41,972,790 instructions as on the interpreter, with every thread and every mapped page agreeing, and spends about 0.3 s in the game's own code against 1.6 s interpreted. [`docs/lifter.md`](docs/lifter.md) describes it and what it measured.

The repository also holds the project's guardrails, its build skeleton, its architecture and decision records, and verified notes on the cartridge formats. Lifting the 133 CRO modules is the next piece of work.

The design is documented in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the decisions behind it in [`docs/adr/`](docs/adr/), and the formats in [`docs/formats/`](docs/formats/). Those documents mark what exists against what is planned.

## Scope

Hexlith targets exactly one title: Pokémon X, program ID `0004000000055D00`, product code `CTR-P-EKJA`, base version with no update applied.

Pokémon Y shares most of the same engine, and the design keeps a second version cheap to add, but it is not owned, not tested and not supported. No other game is claimed.

## What this project does not do

It does not distribute the game, any part of it, or anything derived from it. The repository contains no cartridge image, no extracted content, no assets, no disassembly and no lifted code. See [ADR 0007](docs/adr/0007-no-game-data-in-the-repository.md) for how that is enforced rather than merely intended.

It does not handle encryption keys and implements no decryption. It works only with an already decrypted image of a cartridge you own. An encrypted partition is reported as an error, which is a deliberate limit and not a missing feature.

It is not affiliated with or endorsed by Nintendo, Game Freak or The Pokémon Company.

## Building

Requires a C++20 compiler, CMake 3.25 or later, Ninja, OpenSSL, Vulkan, glslang, SDL3 and GoogleTest. On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build gtest-devel openssl-devel ccache pre-commit \
    vulkan-headers vulkan-loader-devel glslang-devel sdl3-devel \
    mesa-vulkan-drivers vulkan-validation-layers
```

```sh
scripts/install-hooks.sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

[`AGENTS.md`](AGENTS.md) covers the conventions, the workflow and the commit rules, and the tools the oracle comparisons need beyond the build.

## Running it

You need a decrypted image of your own Pokémon X cartridge. The tools read it from a ROM directory and write everything derived from it into a work directory, both outside this repository. Name them once in `~/.config/hexlith/workspace.conf`:

```
rom_dir = /path/to/rom
work_dir = /path/to/work
```

`--rom-dir` and `--work-dir`, or `HEXLITH_ROM_DIR` and `HEXLITH_WORK_DIR`, do the same for one run. [ADR 0008](docs/adr/0008-workspace-configuration.md) has the rules. Then, from the build directory:

```sh
tools/hexlith-extract/hexlith-extract               # unpack the cartridge into the work directory
tools/hexlith-analyze/hexlith-analyze               # find the code and the functions
tools/hexlith-boot/hexlith-boot --window            # run it, interpreted, in a window
```

To run the main executable as lifted code, write it out, build it, and give `hexlith-boot` the library:

```sh
tools/hexlith-lift/hexlith-lift --emit              # into <work>/<title>/lift
../../scripts/build-lifted.sh <work>/<title>/lift   # about six minutes on eight cores
tools/hexlith-boot/hexlith-boot --window --lifted <work>/<title>/lift/build/libhexlith_lifted.so
```

To play it, draw with the Vulkan backend, run the lifted library, and raise the limit of two billion instructions after which a run otherwise stops:

```sh
tools/hexlith-boot/hexlith-boot --window --backend vulkan --device <name> \
    --lifted <work>/<title>/lift/build/libhexlith_lifted.so \
    --limit 18446744073709551615
```

Without `--backend vulkan` the window is drawn by the reference renderer, which works in software one pixel at a time and is far slower. `--device` chooses the Vulkan device by a part of its name. `llvmpipe` needs no GPU and is what the measurements so far were made with; a GPU with a Vulkan driver should draw faster. The save data is kept under `<work>/<title>/save` between runs, and `--new-game` starts without it and writes none. If the lifter changes, emit and build the library again, or the run uses lifted code that no longer matches it.

Every tool takes `--help`.

## License

Original contributions are licensed under the GNU General Public License, version 3 or, at your option, any later version. The full text is in [`LICENSE`](LICENSE).

Third-party components keep their own licenses and notices. See [`LICENSE_SCOPE.md`](LICENSE_SCOPE.md) for what the license does and does not cover, and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for origins and attributions.

## AI assistance

Hexlith is written almost entirely by Claude (Anthropic), working through Claude Code. That covers nearly all of the code, the tests, the documentation and the analysis of the cartridge formats. The project maintainer sets the goals and the scope of each phase, decides or delegates the design questions that come up, keeps the engineering standards, and runs the game under an emulator to capture the traces the work is checked against.

Commits Claude worked on name it as co-author, and the whole codebase should be read as AI-written work under human direction. Generated code and generated documentation may contain errors, including confident ones. Treat statements here as claims to verify rather than as authority, particularly hardware behaviour, container layouts and addresses, which are cited to their sources so you can check them yourself.
