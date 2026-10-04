# Working on Hexlith

Conventions, workflow and rules for anyone working in this repository, human or otherwise. Read this before the first commit.

## What Hexlith is

A static recompilation of Pokémon X for the 3DS, targeting native execution on PC. The ARM11 code is lifted to C++, the Horizon kernel and its services are reimplemented at a high level, PICA200 graphics are translated to Vulkan, and the DSP is emulated in software.

This is a long-term experiment. Correctness comes before speed, and a verified fact comes before a plausible one.

## The one rule that cannot be bent

**Nothing derived from the game ever enters this repository.** No cartridge image, no extracted content, no CRO module, no disassembly, no lifted or generated code, no assets.

Facts about the game are fine and are the point of the GameProfile: program IDs, product codes, hashes, addresses, segment layouts, symbol names. Bytes from the cartridge are not, and neither is disassembly or decompiled source.

Three layers enforce this, and all three are expected to stay green:

1. `.gitignore` excludes binary payloads wholesale rather than by exception.
2. `scripts/check_no_game_data.py` runs from the pre-commit hook on staged files and checks container signatures, reserved extensions, binary content, file size, entropy, and text that looks like a disassembly or a hex dump.
3. CI runs the same script across every tracked file, so skipping the hooks does not skip the check.

If the guard rejects a file you believe belongs here, add it to `scripts/game-data-guard-allowlist.txt` with the reason above the entry. Extension and container-signature matches are not exemptible. Game content is never a valid reason.

## Where the game data lives

Outside the repository, in a workspace directory. On the development machine that is `~/recomp-data`, with `rom/` holding the cartridge image and `work/` holding everything derived from it.

Tools read those locations from configuration and never hardcode them. Precedence is command-line argument, then environment variable (`HEXLITH_ROM_DIR`, `HEXLITH_WORK_DIR`), then the config file at `$XDG_CONFIG_HOME/hexlith/workspace.conf`. No default ever points inside the repository.

The reference ROM is read-only input. Never modify it, move it, or write anything next to it.

## Language

Conversation with the maintainer happens in Spanish. Everything written into the repository is in English: code, identifiers, comments, documentation, commit messages, test names.

## Setup

```sh
scripts/install-hooks.sh        # once after cloning, this is not optional
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets are the local path. CI configures explicitly with `-DCMAKE_CXX_COMPILER` and `-DHEXLITH_WERROR=ON` so that both compilers get covered without a preset for each combination.

`HEXLITH_WERROR` is off by default locally and on in CI, so a warning does not block exploratory work but does block a merge. Build with the `ci` preset to get CI's strictness locally.

`clang-tidy` is not run by CI, but the configuration is kept correct and the tree is kept clean against it: `clang-tidy -p build/ci $(git ls-files '*.cpp' '*.h')`.

Two directories turn off `readability-magic-numbers` in a `.clang-tidy` of their own, with the reason in the file. A decoder is written in bit positions and its tests in instruction encodings, and naming those would hide what the line says.

GoogleTest comes from the system (`gtest-devel` on Fedora). Configure with `-DHEXLITH_FETCH_GOOGLETEST=ON` to download it instead. A plain build never touches the network.

OpenSSL's development package (`openssl-devel` on Fedora) is required, not optional. CMake fails at configure time without it.

## Architecture

Dependencies point inward. A layer may use the one below it and never the one above.

```
tools  ->  adapters  ->  ports  ->  application  ->  domain  ->  core
```

- `src/core` holds small utilities: a `Result` type, bounds-checked byte reading, SHA-256. Its one external dependency is OpenSSL's libcrypto, linked privately by the SHA-256 wrapper. ADR 0009 records why.
- `src/domain` holds pure logic: the ARM model, the memory map, kernel abstractions, the CTR container and CRO formats, the analysis types, the GameProfile. No I/O lives here, which is what makes it testable with synthetic data.
- `src/application` holds use cases such as extract, validate and analyze.
- `src/ports` holds the interfaces the outside world is reached through: `RomSource`, `BlobSink`, `GpuBackend`, `AudioSink`, `InputSource`.
- `src/adapters` holds the implementations of those ports: the filesystem, Vulkan, SDL.
- `tools` holds the command-line programs: `hexlith-extract`, `hexlith-analyze`, `hexlith-arm-dump`, which prints what the decoder makes of a run of words and exists to be compared with another disassembler, `hexlith-arm-run`, which does the same for the interpreter, `hexlith-cro-dump`, which prints what the CRO reader makes of each module so that it can be checked against Azahar's log, and `hexlith-cro-link`, which prints what the linker writes so that it can be checked against Azahar's memory.

The abstraction belongs at system boundaries. Hot paths, meaning lifted code, memory access and GPU command processing, avoid per-instruction virtual dispatch and use static polymorphism with flat data structures instead. Every such exception is documented where it is made.

Anything specific to one game sits behind `GameProfile`. Core logic never asks which game is running, it asks the profile. Horizon services implement a common interface and live in a registry.

Decisions with consequences are recorded in `docs/adr/` rather than argued again later. `docs/formats/` records the container and module formats with a citation for every field.

## Writing code a human can read

Names describe the thing. Functions do one job and stay short. Nesting stays shallow. There are no clever tricks and no magic numbers: a constant gets a name and a comment citing where the value came from.

Comment the why, not the what. A comment earns its place when intent is not obvious from the code, or when the hardware or a file format does something surprising. A comment that restates the next line is noise.

Formatting is not a matter of taste. `clang-format` owns it and the pre-commit hook applies it.

Generated code is held to the same standard within reason: stable symbol names taken from the symbols file, original guest addresses as annotations, consistent formatting, structured control flow where the original allows it. Never obfuscated, never a wall of opaque macros.

Never invent a format, an address or an offset. Verify it against 3dbrew, the Azahar source, or analysis done in this project, and record which one in `docs/formats/`.

## Tests

Unit tests per layer, built on synthetic data only. A test never reads game content, which is both a licensing rule and a practical one: a test that needs a 2 GB cartridge is a test nobody runs.

Synthetic fixtures are generated in code at build time. They are never checked in as files.

Tests go in the same commit as the behaviour they cover.

Unit tests are not enough for a decoder, because the mistakes that matter there are the ones nobody thought to write a case for. The ARM and Thumb decoders are checked against Capstone over millions of random words and over the reference image, by `scripts/compare_arm_decoder.py`. Capstone is a development dependency and nothing else: the build does not use it and the tests do not need it. `docs/arm-decoder.md` records what that comparison found.

The interpreter is held to the same standard against Unicorn, by `scripts/compare_arm_interpreter.py`, one random state and one instruction at a time. Unicorn is installed with pip into a virtual environment and is a development dependency in the same way. Run both comparisons after any change to `src/domain/arm`; `docs/arm-interpreter.md` records the results and what they found.

Golden-trace differential testing against Azahar, over whole scenes rather than single instructions, is planned for when the interpreter can run the game far enough to have scenes. It is not a substitute for unit tests.

## Commits

The history is a sequence of small, logical increments, each building on the one before it. Every commit builds and passes the tests, which is what makes `git bisect` useful.

Inside a feature the usual order is: preparatory refactor, domain types, use case with its tests, adapter, wiring, documentation.

Subject lines are in Title Case, imperative, at most 72 characters, with no trailing full stop and no `type(scope):` prefix. Conventional Commits earns its keep when tooling reads the prefixes, and nothing here does.

```
Add Game Data Guard with Self-Tests
Parse the ExeFS Header
Fix the RomFS Level 3 Offset Calculation
```

Bodies are soft wrapped, one line per paragraph, with a new paragraph when the topic changes. Write one when the reason for the change is not obvious from the diff. Describe why, not what.

There are no WIP commits and no commits that fix the previous commit. Amend or fix up unpushed work instead.

A commit that must break the build or the tests is allowed only when there is no way around it. Mark it with a `Build-Break: <reason>` trailer and restore green in the very next commit. `scripts/verify-history.sh` honours the trailer and rejects two in a row.

### Commit locally only

Never push, force-push or rewrite pushed history without asking the maintainer first. Commits are authored under the maintainer's git identity.

## Verifying the history

```sh
scripts/verify-history.sh                  # commits not on the upstream branch
scripts/verify-history.sh HEAD~10..HEAD    # an explicit range
scripts/verify-history.sh -k main..branch  # report every failure, not just the first
```

Each commit is checked out into a throwaway worktree and built there, so the script cannot damage the history it is checking and the working tree stays usable while it runs. Commits from before the build existed are reported as skipped, not passed.

With `ccache` warm this is fast enough to run before every push.

## AI assistance

Hexlith is written almost entirely by Claude (Anthropic) through Claude Code. The maintainer sets the goals and the scope of each phase and decides or delegates the design questions. Generated code may contain errors.

Disclosure is at project level, in `README.md`.
