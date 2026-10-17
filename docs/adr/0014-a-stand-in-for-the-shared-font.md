# 0014. The shared font is a stand-in built from the format

Status: Accepted
Date: 2026-10-17

## Context

Every 3DS has one system font, which applications draw their text with. It is a system title on the console's own storage, not part of any cartridge, and APT hands it to an application through GetSharedFont: a block of shared memory with a short header and the font after it, already relocated to the address it is mapped at.

The reference title asks for it early in its boot, before its first module loads, and stops with a Break as soon as GetSharedFont answers with an error. So the boot needs a font of some kind, and the cartridge does not have one.

[ADR 0007](0007-no-game-data-in-the-repository.md) rules out anything taken from the console as firmly as anything taken from the cartridge, and the maintainer decided that a system archive is first answered as not found and, when the game cannot carry on without it, replaced by something generated in code. Never a dump.

## Options

**Answer with an error and patch around the Break.** Keeps every byte out of the repository and makes Hexlith run code the console would not: the game decides what to do with a missing font, and that decision is a crash.

**Load the font from a dump of the user's console, from the workspace.** What Azahar does when the user supplies one. Correct text, and a dependency on a file most people do not have, which the boot would then need just to get past its first steps. It can still be added later as an option.

**Build a font in code from the format alone.** A valid font in the BCFNT format, with one blank glyph every character maps to. Holds nothing Nintendo made. Text drawn with it is blank.

## Decision

The third. `services::WriteStandInFont` writes the 0x80-byte header APT puts before the font, then a relocated font, magic CFNU, with the four sections the game's font library reads: FINF for the metrics, TGLP for one sheet of one 8 by 8 four-bit alpha glyph, CWDH for its width and CMAP mapping the space character to it, with glyph 0 as the default for every character that is not mapped. The layout follows 3dbrew's BCFNT page and the structures Azahar reads. The block is 0x332000 bytes, as large as APT's, and comes from the system region, which puts it at `0x18000000` as on the console.

## Consequences

The boot gets past the font, and the game runs the code it would run with a real one.

Every character the game draws with the system font is blank. That is visible only once Hexlith draws anything, and it can be fixed then by reading a font the user dumps into the workspace, as an option and never a requirement.

A game that measures text against glyphs it expects to exist, or that looks up a glyph by index past zero, would see a font it was never shipped with. Nothing in the boot so far does, and the stand-in is the first thing to suspect if text layout misbehaves later.
