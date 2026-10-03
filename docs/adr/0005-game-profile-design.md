# 0005. GameProfile splits compiled identity from text tables

Status: Accepted
Date: 2026-10-03

## Context

Everything specific to one title and version needs to live in one place so that core logic never asks which game is running. That covers the program ID, the product code, content hashes, segment addresses, the module list, and eventually hundreds of thousands of symbol and function records.

Only Pokémon X, program ID `0004000000055D00`, base version, is owned and testable. Pokémon Y shares most of the engine and may be added later. The design has to make that a matter of adding data rather than editing logic.

Those two kinds of data are not alike. There are a few dozen identity and layout facts, and they are structural: a wrong segment address breaks everything. There are potentially hundreds of thousands of analysis records, they change constantly, and they are edited by hand.

## Options

**Everything compiled in**, as `constexpr` data in C++. Type checked, no parser, no runtime failure mode. Impractical for large tables that change often, since every edit is a rebuild.

**Everything external**, in TOML or JSON. Flexible, and needs a parser, a dependency, and a validation layer. A typo becomes a runtime error.

**Split by kind.** Identity and layout compiled in, bulk tables external.

## Decision

Split.

Identity and layout live as `constexpr` data in one translation unit per game version, under `src/domain/profile`, registered in a `GameProfileRegistry`. This covers the program ID, product code, internal application title, NCCH version, content hashes, the three segment addresses and sizes, stack and BSS sizes, the declared service list and the module list.

Bulk analysis tables live as line-oriented text under `data/profiles/<program-id>-v<version>/`, in the format [ADR 0006](0006-analysis-layer-format.md) describes.

## Why

The compiled half gets type checking and needs no parser, which matters because these are the values that break everything if wrong. A missing field is a compile error rather than a silent default. There is no configuration file for a user to corrupt and no error path for a malformed profile to test.

The text half is diffable, editable by hand, and regenerable without rebuilding. Symbol work is iterative: run the analysis, read the result, rename twenty functions, run again. Recompiling for that would make the work unpleasant enough that it would not get done.

These values are facts about the game and not content from it, so they belong in the repository. Hashes, addresses and symbol names are explicitly permitted by the project rule; bytes and disassembly are not. See [ADR 0007](0007-no-game-data-in-the-repository.md).

## Adding a version

One new translation unit with the identity constants, and one new directory of analysis data. No change to the lifter, the runtime, the extractor or the service layer.

The registry is keyed by program ID and version, so the extractor can validate an unknown image, report which profile it does not match, and refuse rather than guess. Selecting the wrong profile silently is a worse outcome than refusing to run.

## Consequences

A new game version requires a rebuild, which is correct: it is new code in the project, not configuration.

The registry has to be populated without relying on initialisation order across translation units, so profiles are registered explicitly from one list rather than through static constructors.

Nothing outside `src/domain/profile` may hard-code a program ID, an address or a hash. That is a review rule, and it is the rule that keeps the second title cheap.
