# 0001. C++20 and CMake, for tools as well as the runtime

Status: Accepted
Date: 2026-10-03

## Context

Hexlith needs a language for the runtime, the lifter and the offline tools. The runtime has to be fast and has to interoperate with Vulkan and SDL. The offline tools have to parse binary container formats and emit large amounts of source code.

TriAevum, the closest prior art, uses C++ for its runtime and Python for its tooling, including its cartridge extraction.

The development machine has gcc 16.2, clang 22.1, CMake 4.3, Ninja, ccache and GoogleTest 1.17 installed. It does not have pytest.

## Options

**C++20.** Supported completely by both installed compilers, and by MSVC and Apple clang if the project ever needs them.

**C++23.** Also supported by the installed compilers. Of its additions the one Hexlith would actually use is `std::expected`, since error handling in the parsers wants exactly that shape.

**Tools in Python, runtime in C++.** Faster to iterate on the tools, and the approach TriAevum took.

**Everything in C++.** One language, one build, one test framework.

## Decision

C++20 for everything, built with CMake and Ninja, with the tools in the same build as the runtime.

`std::expected` is replaced by a small `core::Result` in `src/core`. That is a few dozen lines against the cost of raising the compiler floor for every future contributor.

Python is used only for throwaway analysis inside the workspace directory, and for the repository guard scripts, which have to run from a git hook and are written against the standard library alone so that they need nothing installed.

Tests use GoogleTest from the system. Downloading it is opt-in through `HEXLITH_FETCH_GOOGLETEST`, so a plain build never reaches the network.

## Why not Python for the tools

The container and CRO parsers are needed in two places: offline by the extractor, and at runtime by the `ldr:ro` reimplementation when the game loads a module. A Python extractor plus a C++ runtime parser means the same format is implemented twice by different people at different times.

Those two implementations would eventually disagree. The symptom would be a wrong address in generated code, which surfaces as the game calling the wrong function long after the mistake, and that is among the most expensive classes of bug this project can create. One implementation in `src/domain`, used by both, removes the possibility.

This overrides the original plan, which had the Phase 1 tool following the existing Python script.

## Consequences

Phase 1 costs more than it would in Python. Binary parsing in C++ needs explicit bounds checking and careful integer handling where Python would have raised an exception by itself. The warning policy is set up accordingly: `-Wconversion` and `-Wsign-conversion` are on, because a silent narrowing in a parser is a wrong value rather than a diagnostic.

Nothing in the project depends on a Python package, which keeps the hooks and CI working on a bare system.

Raising to C++23 later is a one-line change if a reason appears. Lowering from C++23 would not have been.
