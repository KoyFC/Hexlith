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

For a CRO module they are the offsets the module itself uses, because the loader decides where a module lands and the analysis cannot know it. [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) explains why the project refuses to assume otherwise.

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

### `pointers.txt`

| Field | Values |
| --- | --- |
| `target` | The code address held at this location |

The main executable has no relocation table, so an absolute address sitting in its data is indistinguishable from an integer until something proves otherwise. Each one found is recorded with how it was found. CRO modules carry their own relocation tables and theirs are read from the module instead.

## `source=`

Every record carries one.

| Value | Meaning |
| --- | --- |
| `crs-export` | A named export table of a CRO-family module |
| `exheader` | The extended header: the entry point, the segment bounds |
| `relocation` | A relocation table, which names an address without saying what is at it |
| `ghidra` | Imported from a decompiler project |
| `heuristic` | This project's own analysis |
| `manual` | A decision a person made |

A regeneration pass replaces records it can derive from the cartridge again, which is the first four, and leaves `ghidra` and `manual` alone. The rule is not about how much a source is trusted. It is about whether the record can be produced a second time: rerunning the analysis reproduces everything that came out of the image, and a name from a person is the only copy there is.

## Stability

Three properties hold, and the tests hold them.

Records are sorted by address, then by the rest of the record, so that output does not depend on the order the analysis found things in. Numbers are fixed width. A file written twice from the same facts is identical byte for byte.

The point is the diff. A pass over two million instructions that reshuffled its own output would make version control useless for reviewing what the pass did, and reviewing what it did is the only way to tell an improvement from a regression.
