# 0008. Workspace paths come from configuration, never from code

Status: Accepted
Date: 2026-10-03

## Context

Every tool needs to find the cartridge image and somewhere to put what it extracts. On the development machine those are `~/recomp-data/rom` and `~/recomp-data/work`, but nothing in the project may assume that.

The cartridge image is read-only input. It is the user's own copy of a game they own, it is 2 GB, and it must never be modified, moved or copied into the repository.

## Options

**Hard-coded paths.** Works on one machine.

**A single environment variable** pointing at a workspace root, with a fixed layout inside it.

**A configuration file** in the standard location for the platform.

**All of the above with a precedence order.**

## Decision

Precedence, highest first:

1. A command-line argument
2. `HEXLITH_ROM_DIR` and `HEXLITH_WORK_DIR`
3. `$XDG_CONFIG_HOME/hexlith/workspace.conf`, falling back to `~/.config` when `XDG_CONFIG_HOME` is unset

No default points inside the repository. When no source provides a path, the tool reports which sources it consulted and exits rather than inventing a location.

The configuration file is a line-oriented `key = value` text file, read by the same code that reads the analysis artefacts, so the project gains no parser and no dependency for it.

## Why this order

A command-line argument wins because a one-off run against a different image should not require editing anything. Environment variables come next because that is how CI and scripts pass this kind of thing. The configuration file is last because it is the persistent convenience, and the one most likely to be stale.

Reporting the sources consulted on failure matters more than it sounds. The failure mode being avoided is a tool that silently writes into the current directory, which is how extracted content ends up next to the source in the first place.

## Treatment of the ROM

The image is opened read-only and never written, renamed or moved. The extractor refuses a symbolic link, so that a path cannot be redirected after it was validated.

Hexlith handles no keys and implements no decryption. An encrypted partition is reported as an error rather than worked around, which is a deliberate limit on what the project does and not an unimplemented feature.

## Consequences

Every tool takes the same workspace arguments, so the resolution logic lives in one place in `src/application` rather than in each `main`.

Tests never touch the workspace. They build synthetic images in a temporary directory, which is also what makes the test suite runnable on a machine with no cartridge image at all.
