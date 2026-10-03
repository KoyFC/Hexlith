# 0007. No game-derived data in the repository, enforced mechanically

Status: Accepted
Date: 2026-10-03

## Context

Hexlith works on a copyrighted commercial game. The repository is private now and may become public.

The distinction that matters is between facts about the game and content from it. A program ID, a segment address, a content hash and a function name are facts, and the project cannot work without recording them. A cartridge image, an extracted file, a CRO module, a disassembly listing and lifted output are content, and none of it may ever be committed.

The risk is not a deliberate commit. It is `git add -A` run in a directory where an extraction was tested, or a debugging session that pasted a hex dump into a note, or a generated file that landed inside the tree because a path was wrong.

TriAevum shows what happens when this is not mechanical from the start. It carries 99 MB of generated binary data committed as C++ headers, and its `.gitignore` is inherited from an unrelated Nintendo 64 project, so it excludes `*.z64` and `*.png` while matching nothing that a 3DS extraction produces.

## Options

**Discipline and review.** Depends on nobody ever being careless.

**One `.gitignore`.** Stops the expected cases and is bypassed by `git add -f`, which is exactly what someone does when they believe a file is being ignored wrongly.

**Layered mechanical enforcement.** Patterns, a content-aware check in a git hook, and the same check again in CI.

## Decision

Three layers, all expected to stay green.

**`.gitignore`** excludes binary payloads wholesale rather than by exception: cartridge and container extensions, CRO modules, extracted content, key filenames, and `*.bin` as a class. Synthetic test fixtures are generated into the build directory at build time, so nothing legitimate needs to be a tracked binary.

**`scripts/check_no_game_data.py`**, run from the pre-commit hook on staged files, inspects content rather than names:

- container magic values at the offset each format places them, `NCSD` and `NCCH` at 0x100, `CRO0` and `FIXD` at 0x80, `IVFC`, `CRR0` and `SMDH` at 0
- reserved extensions
- any file containing a NUL byte
- any file over 256 KiB
- Shannon entropy above 7.2 bits per byte, which separates compressed or encrypted data from source and prose
- text holding eight or more lines that parse as a disassembly listing or a hex dump

**CI** runs the same script across every tracked file, so skipping the hooks does not skip the check.

An allowlist at `scripts/game-data-guard-allowlist.txt` exempts named paths from the size, binary and entropy checks, with the reason recorded above each entry. The extension and magic checks cannot be exempted.

## Why these checks

Each one catches a different real mistake. The magic check catches an extracted partition regardless of what it was named. The NUL byte check catches binary content generically, including formats nobody anticipated. The size check is a tripwire, since a hand-written file over 256 KiB is almost certainly not hand-written. The entropy check catches compressed data that has no recognisable header. The text checks catch the case the other layers miss entirely, which is game content pasted into a document as text.

The checks are blunt on purpose. A false positive costs one line in the allowlist. A false negative puts copyrighted code in a public repository, and git history makes that permanent.

## Verification

All three layers were tested adversarially rather than assumed to work. A file with `NCCH` at offset 0x100 was committed with `git add -f` to bypass the ignore rules, and the pre-commit hook rejected it on both the magic and the binary content check. The guard carries thirteen crafted cases in a `--self-test` mode that runs whenever its own source changes.

## Consequences

Any legitimate binary file needs a deliberate allowlist entry, which is the intended friction.

The extracted workspace must never be inside the repository. [ADR 0008](0008-workspace-configuration.md) covers how tools find it.

The guard scripts use only the Python standard library, so they work from a git hook on a machine with nothing installed.
