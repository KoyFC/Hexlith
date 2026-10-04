# 0011. Only records nothing can derive are committed

Status: Accepted. Amends [ADR 0005](0005-game-profile-design.md) and [ADR 0006](0006-analysis-layer-format.md) on where the analysis tables live.
Date: 2026-10-04

## Context

ADR 0005 put a profile's bulk analysis tables under `data/profiles/<program-id>-v<version>/`, and ADR 0006 gave them their format. Both were written before anything produced a table.

`hexlith-analyze` now produces four, and they turned out not to be what those records pictured. Almost every record in them carries `source=heuristic` or `source=crs-export`, which means a tool derived it from the cartridge and will derive it again, identically, in under half a second. On the reference image they hold 461 symbols, 11,941 functions, 16,075 regions and 6,198 pointers, and three of the four files are several hundred kilobytes.

Committing them would cost three things. Every change to the analysis would show up as a diff of thousands of lines that says nothing a rerun would not. The game data guard rejects files of that size, so each would need an allowlist entry. And a table in the repository would go stale the moment the analysis improved, which has already happened: decoding floating point added 117 functions and changed 579 lines of the function table.

What the tables were meant to protect is the work a person puts in: a function recognised and named, a jump table found by hand, a region reclassified. That is the part regeneration cannot recover, and it is a small fraction of each table.

## Options

**Commit the tables as generated**, as ADR 0005 says. Simple, and every objection above applies.

**Commit nothing.** Human work would live only in a workspace, which is outside version control and unshared.

**Commit only what nothing can derive.** The repository holds the records a person entered, `source=manual`, and in time those imported from a decompiler, `source=ghidra`. The derived records stay in the workspace.

## Decision

Commit only what nothing can derive.

`data/profiles/<program-id>-v<version>/` holds, in the format of ADR 0006, the records whose provenance is not something a tool in this repository produces from the cartridge. The full tables are written into the workspace, beside the extraction, as they are now.

When a run of `hexlith-analyze` builds the tables, it reads the committed records as an overlay and merges them by the provenance rules that already exist, so that a person's record wins over a derived one for the same address. The overlay is read from a directory named on the command line, because a tool does not find the repository on its own any more than it finds the workspace.

Nothing is committed until the first such record exists. The overlay reader is written then, with its tests, rather than now for a directory that would be empty.

## Why

The repository is for what cannot be reproduced. A derived table can always be reproduced from the cartridge by the tools in the repository, so committing it records nothing new, and the same reasoning keeps extracted content out.

Provenance is what makes the split mechanical rather than a judgement made file by file. ADR 0006 required the `source` field on every record for exactly this kind of decision, and here it decides which records are worth keeping.

## Consequences

The committed files are small, change only when a person changes them, and are reviewable line by line.

A fresh clone has no analysis tables until `hexlith-analyze` runs against a workspace. That was already true of everything else derived from the cartridge.

The 461 symbol names from `static.crs` are derived, so they are not committed either. They come back on every run.

ADR 0005's split between compiled identity and text tables stands. Only the location of the derived part of the text tables changes.
