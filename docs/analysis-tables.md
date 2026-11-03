# Analysis tables

What lifting needs that the cartridge does not state: where each function begins and ends, which bytes are instructions, which are data, and what to call things. [ADR 0006](adr/0006-analysis-layer-format.md) chose line-oriented text for it and gives the reasoning. This note is the grammar, so that the files can be edited by hand without reading the parser.

These are Hexlith's own files, not the game's. The notes under [`formats/`](formats/README.md) describe formats someone else defined and cite a source for every field; this one defines a format and is the source.

## Lines

```
# Symbols: an address, how far it runs, what it is and its name.
format version=1
0x0012A4B0 size=0x000000C4 kind=func name=FieldMain source=crs-export
```

A line is a comment, a blank line, the format line, or a record.

A comment runs from the first `#` on a line that holds nothing but whitespace before it. There are no trailing comments: a `#` after a field is part of that field.

The format line comes before any record and appears once. A reader that does not know the version refuses the file rather than skipping what it cannot read, which is why the version is a line of its own and not a comment.

A record is an address, then `key=value` fields, separated by single spaces. A value holds no whitespace. Every table can promise that because every value is a number, a keyword or a symbol name, and the one it cannot promise it does not have: there is no comment or description field, and a name with a space in it is not a name the lifter could emit anyway.

Addresses and sizes are hexadecimal behind `0x`. A writer emits exactly eight uppercase digits. A reader accepts one to eight, in either case, so that a hand-written `0x100000` means what it looks like and is rewritten as `0x00100000` the next time the file is generated.

A size of `0x00000000` means the extent is not known yet. That is the state a symbol seeded from an export table starts in: the table says where a symbol is and not how far it runs.

## Which address space

Addresses in a table belong to one module, and a table never mixes two.

For the main executable they are virtual addresses, which that image fixes: `0x00100000` is the first byte of its text segment.

For a CRO module they are offsets into the module's file, because the loader decides where a module lands and the analysis cannot know it. [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) explains why the project refuses to assume otherwise. The one part of a module that is not in its file is its BSS, which is given the first page boundary past the end of the file, so that no address in it is also an offset into the file. Each module's tables are written to `modules/<name>/` beside the main executable's.

## The four tables

### `symbols.txt`

| Field | Values |
| --- | --- |
| `size` | Hexadecimal, `0x00000000` when not known |
| `kind` | `func`, `data`, `unknown` |
| `name` | The symbol name |

`unknown` is not laziness. An export table gives an address and a name and says nothing about what is at it, so a seeded symbol has to be able to admit that until analysis settles it.

Two records may share an address. An alias is one reason, and a name an export table gives to the first instruction of a function already named is another.

### `functions.txt`

| Field | Values |
| --- | --- |
| `size` | Hexadecimal, `0x00000000` when not known |
| `isa` | `arm` or `thumb` |
| `alt-entries` | Comma-separated addresses, the field left out when there are none |

`isa` is required. Which encoding a stretch of code is in is not a property of its address: the low bit of a `BX` target carries it, and nothing in the image states it. A wrong answer decodes garbage that still looks like instructions.

`alt-entries` lists the other addresses the lifter may enter the function at. Hand-written ARM routines share a tail between two entry points, and a compiler produces one when it folds two functions whose endings agree. Each entry has to lie inside the function and cannot be its first address.

### `regions.txt`

| Field | Values |
| --- | --- |
| `size` | Hexadecimal |
| `kind` | `code`, `data`, `jump-table`, `padding` |
| `isa` | `arm` or `thumb`, on a code region and only there |

`jump-table` is not data that happens to sit in code. It is read by a PC-relative load followed by a computed branch, it lives in the middle of a function, and decoding it as instructions produces plausible nonsense, so it gets a kind of its own.

A jump table region is exactly the table and nothing more. In ARM it starts eight bytes after the `ldr pc, [pc, rm, lsl #2]` that goes through it, which is what the program counter reads, and holds one absolute code address a word. The word between the load and the table is the branch to the default case and is code. [ADR 0023](adr/0023-switches-and-tables-of-functions-are-recognised-by-shape.md) has how the walk recognises one.

### `pointers.txt`

| Field | Values |
| --- | --- |
| `target` | The code address held at this location |

The main executable has no relocation table, so an absolute address sitting in its data is indistinguishable from an integer until something proves otherwise. Each one found is recorded with how it was found. A CRO module carries its own relocation tables, and in a module's `pointers.txt` every record comes from them, marked `source=relocation`: a relocated word that the loader points into the module's own code. Nothing in a module's table is a guess from a word's value.

## `source=`

Every record carries one.

| Value | Meaning |
| --- | --- |
| `crs-export` | A named export table of a CRO-family module |
| `exheader` | The extended header: the entry point, the segment bounds |
| `relocation` | A relocation or import table of a CRO module, which names an address without saying what is at it |
| `observed` | An instruction a run of the title executed, in the instruction set it ran in, from `observed/executed.txt` |
| `ghidra` | Imported from a decompiler project |
| `heuristic` | This project's own analysis |
| `manual` | A decision a person made |

A regeneration pass replaces records it can derive from the cartridge again, which is every source but two, and leaves `ghidra` and `manual` alone. An `observed` record counts as derived: running the title with the same input produces it again. The rule is not about how much a source is trusted. It is about whether the record can be produced a second time: rerunning the analysis reproduces everything that came out of the image, and a name from a person is the only copy there is.

## Stability

Three properties hold, and the tests hold them.

Records are sorted by address, then by the rest of the record, so that output does not depend on the order the analysis found things in. Numbers are fixed width. A file written twice from the same facts is identical byte for byte.

The point is the diff. A pass over two million instructions that reshuffled its own output would make version control useless for reviewing what the pass did, and reviewing what it did is the only way to tell an improvement from a regression.

## What a run observed

The four tables are what the analysis concluded. `hexlith-boot --record-executed` writes a fifth file that is not a conclusion but an observation: `observed/executed.txt` beside the extraction, one record per instruction the run executed in the main executable's text segment.

```
format version=1
0x00100000 isa=arm count=3
```

`isa` is the instruction set the processor was in when it executed the address, and `count` is how many times. A run that executes the same address in both instruction sets has two records. The file is derived from a run and not edited by hand, and two runs of the same input give the same file.

The main executable's file holds its text segment, by address. What a run executed in a module is a file of its own, `observed/modules/<name>/executed.txt`, one for each module that ran, in the same grammar. A module's addresses depend on where the game had it loaded, so the records are offsets from the module's start: the file offsets that module's analysis tables use, and only in its code segment. A module loaded at two addresses, in one run or in two, is one table, since an offset means the same instruction wherever the module is, and the name is the one in the module's header, the loader's key and the directory of its analysis tables.

`scripts/executed_coverage.py` sets the file against `functions.txt` and `regions.txt`, and with `--modules` every module's file against that module's tables. It answers a different question from the static reach: of the instructions the game executed, how many did the analysis account for.
