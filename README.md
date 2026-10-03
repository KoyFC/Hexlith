# Hexlith

A static recompilation of Pokémon X for the PC.

The goal is to run the game natively rather than emulate it: translate its ARM11 code to C++ ahead of time, reimplement the Horizon kernel and its services at a high level, translate PICA200 graphics to Vulkan, and emulate the DSP in software.

## Status

Early. Nothing is playable and nothing runs.

As of now the repository holds the project's guardrails, its build skeleton, its architecture and decision records, and verified notes on the cartridge formats. The tool that extracts a cartridge image is the next piece of work.

The design is documented in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the decisions behind it in [`docs/adr/`](docs/adr/), and the formats in [`docs/formats/`](docs/formats/). Those documents mark what exists against what is planned.

## Scope

Hexlith targets exactly one title: Pokémon X, program ID `0004000000055D00`, product code `CTR-P-EKJA`, base version with no update applied.

Pokémon Y shares most of the same engine, and the design keeps a second version cheap to add, but it is not owned, not tested and not supported. No other game is claimed.

## What this project does not do

It does not distribute the game, any part of it, or anything derived from it. The repository contains no cartridge image, no extracted content, no assets, no disassembly and no lifted code. See [ADR 0007](docs/adr/0007-no-game-data-in-the-repository.md) for how that is enforced rather than merely intended.

It does not handle encryption keys and implements no decryption. It works only with an already decrypted image of a cartridge you own. An encrypted partition is reported as an error, which is a deliberate limit and not a missing feature.

It is not affiliated with or endorsed by Nintendo, Game Freak or The Pokémon Company.

## Building

Requires a C++20 compiler, CMake 3.25 or later, Ninja and GoogleTest. On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build gtest-devel ccache pre-commit
```

```sh
scripts/install-hooks.sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

[`AGENTS.md`](AGENTS.md) covers the conventions, the workflow and the commit rules.

## Licence

Original contributions are licensed under the GNU General Public License, version 3 or, at your option, any later version. The full text is in [`LICENSE`](LICENSE).

Third-party components keep their own licences and notices. See [`LICENSE_SCOPE.md`](LICENSE_SCOPE.md) for what the licence does and does not cover, and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for origins and attributions.

## AI assistance

Hexlith is developed with substantial help from Claude (Anthropic), used through Claude Code. The project maintainer directs the architecture, sets the engineering standards and reviews everything that enters the repository; the assistant writes code, documentation and analysis against that direction.

This is disclosed once, at project level, rather than per commit: the whole codebase should be read as AI-assisted work under human review. Generated code and generated documentation may contain errors, including confident ones. Treat statements here as claims to verify rather than as authority, particularly hardware behaviour, container layouts and addresses, which are cited to their sources so you can check them yourself.
