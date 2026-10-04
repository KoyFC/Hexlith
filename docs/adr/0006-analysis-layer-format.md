# 0006. Analysis artefacts are line-oriented text with provenance

Status: Accepted. Amended by [ADR 0011](0011-only-underivable-records-in-the-repository.md), which keeps derived records out of `data/profiles/`.
Date: 2026-10-03

## Context

Lifting needs knowledge the cartridge does not state: where each function begins and ends, which bytes are instructions and which are data, which addresses are jump table entries, and what to call things.

That knowledge accumulates for as long as the project runs. Some is derived by automatic analysis, some comes from a decompiler, and some is a person recognising a function and naming it. All three kinds end up in the same tables, and regenerating the automatic parts must not destroy the manual ones.

The scale is 2.2 million instructions. The only symbol information the cartridge gives away is 461 named exports in `static.crs`, plus the exports each CRO module declares.

## Options

**A binary database.** Compact and fast, not diffable, not hand-editable, and it would need its own tooling to inspect.

**SQLite.** Queryable and transactional, opaque in version control, and a dependency.

**JSON or YAML.** Structured and hand-editable in principle. Reordering or reformatting on regeneration produces enormous diffs, and both encourage nesting that makes line-level merging awkward.

**Line-oriented text, one record per line.** Diffable, greppable, mergeable, and parseable with code we own.

## Decision

Line-oriented text, under `data/profiles/<program-id>-v<version>/`. One file per kind of record.

```
0x0012A4B0 size=0x00C4 kind=func name=FieldMain source=crs-export
```

A leading address, then `key=value` fields. Comment lines start with `#`. Records are sorted by address.

Three properties are not negotiable.

**Stable under regeneration.** Sorted by address, with fixed-width hexadecimal and a fixed field order, so that regenerating a file changes only the records that actually changed. A tool that reshuffles its own output makes version control useless for reviewing what it did.

**Editable by hand.** A person renaming a function opens the file and renames it. No tool required.

**Provenance on every record.** The `source=` field says where the record came from: `crs-export` for a CRO export table, `exheader` for the extended header, `ghidra` for imported decompiler output, `heuristic` for automatic analysis, `manual` for a human decision.

## Why provenance matters

It is what makes regeneration safe. A regeneration pass may overwrite records marked `heuristic` and must preserve records marked `manual`. Without the field, either the automatic analysis is never re-run or it silently discards human work.

It also satisfies the project rule that nothing is asserted without a source. A name that came from a guess is labelled as a guess, which keeps a plausible invention from hardening into an assumed fact over months of work.

## Files

| File | Records |
| --- | --- |
| `symbols.txt` | Address, size, kind and name |
| `functions.txt` | Function ranges and entry points, including the secondary entry points the lifter is allowed to dispatch to |
| `regions.txt` | Classification of address ranges as code, data, jump table or padding |
| `pointers.txt` | Locations in data holding absolute code addresses |

The main executable is a fixed-address image and has no relocations, so absolute pointers in its data have to be identified by analysis. CRO modules carry their own relocation tables, so theirs are read from the module and not stored here.

## Consequences

The parser and the writer are ours to write and ours to test, which is a small amount of code with no dependency.

A file of several hundred thousand lines is large but not a problem: it is read once by a tool rather than queried repeatedly, and it compresses well in git since most lines never change.

If querying ever becomes the bottleneck, an index can be generated into the workspace. The text files stay the source of truth, because the source of truth has to be the thing a person can read and edit.

`static.crs` seeds `symbols.txt` with 461 records marked `crs-export`, which is where the analysis layer starts rather than from nothing.
