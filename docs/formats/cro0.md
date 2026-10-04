# CRO modules, CRS and CRR

A CRO is a dynamically loaded code module, the 3DS equivalent of a shared library. The title asks the `ldr:ro` service to load one, and the service maps it, resolves its imports against modules already loaded, and applies its relocations.

This is the format that matters most to Hexlith, because roughly half of the game's executable code lives in CRO modules rather than in the main executable. The lifter cannot treat them the way it treats a fixed-address image.

Sources: `3dbrew CRO0` for the overview, and `azahar/src/core/hle/service/ldr_ro/cro_helper.h` for the field layout, which 3dbrew does not give completely. Observations from the reference image.

## Header

The first 0x80 bytes hold four SHA-256 hashes covering different regions of the module. From 0x80 the header is a flat array of 32-bit fields, so the field at index `n` sits at `0x80 + 4 * n`.

| Offset | Index | Field |
| --- | --- | --- |
| 0x80 | 0 | Magic, `CRO0`, or `FIXD` once the module has been fixed up |
| 0x84 | 1 | Name offset |
| 0x88 | 2 | Next CRO in the loaded list |
| 0x8C | 3 | Previous CRO in the loaded list |
| 0x90 | 4 | File size |
| 0x94 | 5 | BSS size |
| 0x98 | 6 | Fixed size |
| 0x9C | 7 | Unknown, zero |
| 0xA0 | 8 | Unknown segment tag |
| 0xA4 | 9 | OnLoad segment tag |
| 0xA8 | 10 | OnExit segment tag |
| 0xAC | 11 | OnUnresolved segment tag |
| 0xB0 | 12 | Code offset |
| 0xB4 | 13 | Code size |
| 0xB8 | 14 | Data offset |
| 0xBC | 15 | Data size |
| 0xC0 | 16 | Module name offset |
| 0xC4 | 17 | Module name size |
| 0xC8 | 18 | Segment table offset |
| 0xCC | 19 | Segment count |
| 0xD0 | 20 | Export named symbol table offset |
| 0xD4 | 21 | Export named symbol count |
| 0xD8 | 22 | Export indexed symbol table offset |
| 0xDC | 23 | Export indexed symbol count |
| 0xE0 | 24 | Export strings offset |
| 0xE4 | 25 | Export strings size |
| 0xE8 | 26 | Export tree table offset |
| 0xEC | 27 | Export tree node count |
| 0xF0 | 28 | Import module table offset |
| 0xF4 | 29 | Import module count |
| 0xF8 | 30 | External relocation table offset |
| 0xFC | 31 | External relocation count |
| 0x100 | 32 | Import named symbol table offset |
| 0x104 | 33 | Import named symbol count |
| 0x108 | 34 | Import indexed symbol table offset |
| 0x10C | 35 | Import indexed symbol count |
| 0x110 | 36 | Import anonymous symbol table offset |
| 0x114 | 37 | Import anonymous symbol count |
| 0x118 | 38 | Import strings offset |
| 0x11C | 39 | Import strings size |
| 0x120 | 40 | Static anonymous symbol table offset |
| 0x124 | 41 | Static anonymous symbol count |
| 0x128 | 42 | Internal relocation table offset |
| 0x12C | 43 | Internal relocation count |
| 0x130 | 44 | Static relocation table offset |
| 0x134 | 45 | Static relocation count |
| 0x138 | 46 | Fix level 0 barrier |

The field at 0xF8 is the external relocation table, not an import table. During the initial survey it was labelled as one, because the table offsets and counts alternate and the pairs are easy to slip by one. The numbers reported before that was caught were wrong, which is why the index column is in this table.

## Segment table

An array of 12-byte entries at the segment table offset, each holding an offset, a size and a type.

| Type | Meaning |
| --- | --- |
| 0 | Code, executable |
| 1 | ROData, read-only |
| 2 | Data, read-write |
| 3 | BSS, zero-initialised |

A module's segment offsets are relative to the file. The static module's are not, which is the next section.

## The static module, `static.crs`

A CRS is a CRO describing the main executable rather than a loadable module. Its purpose is to let CRO modules link against the code already running, so it carries the exports and the import requirements of the static image without carrying any code of its own.

Its module name is `|static|`, with the pipe characters included, which is how the loader distinguishes it.

Its segment offsets are absolute virtual addresses, not file offsets, because the segments it describes are already mapped. On the reference image they match the extended header code set addresses exactly, which is a useful cross-check that both were read correctly.

**The CRS is the only source of symbol names available to this project**, and it is why the analysis layer has something to start from rather than nothing. See the observations below.

## Relocations

A relocation entry carries a target position, a type, two batch flags and an addend.

| Type | Meaning |
| --- | --- |
| 0 | Nothing |
| 2 | Absolute address |
| 3 | Relative address |
| 10 | Thumb branch |
| 28 | ARM branch |
| 29 | Modify ARM branch |
| 38 | Absolute address, second form |
| 42 | Aligned relative address |

Internal relocations point within the module. External relocations point at a symbol in another module and are resolved when that module is present.

The batch flags matter: relocations are grouped, and the loader resolves a batch as a unit and records whether it succeeded, so that an unresolved import can be patched later when the module providing it loads.

**This is the mechanism that makes CRO the project's hardest problem.** The addresses a module's code uses are not known until load time, and the resolution of a cross-module call depends on which modules happen to be loaded. A lifter that bakes an address into generated C++ is betting that the address will be the same every time. If that bet is wrong, the generated code calls the wrong function and the failure appears far from its cause. ADR 0004 records how Hexlith handles this.

## Positions, and why nothing in a module is an address

A CRO refers to its own contents by a packed 32-bit value: the segment index in the low four bits, the byte offset within that segment in the other 28. Source: `SegmentTag` in `cro_helper.h`.

```
31                                 4   3   0
+----------------------------------+------+
|      offset into the segment     | index|
+----------------------------------+------+
```

Four bits of index is why a module may declare at most sixteen segments, and 28 bits of offset is why one segment cannot exceed 256 MiB. Neither limit is near: the largest segment in the reference image is under a megabyte and no module declares more than six segments.

This is the mechanism behind position independence. Nothing inside a module names an address. Everything names a segment and a distance into it, and the loader turns that into an address once it has decided where the segment goes.

Resolving a position means adding the segment's own offset. `SegmentTagToAddress` in `cro_helper.cpp` refuses two cases, answering address zero for both: a segment index past the end of the segment table, and an offset that reaches the end of its segment or beyond. Hexlith refuses the same two, as failures rather than as address zero.

## Export tables

| Table | Entry | Size |
| --- | --- | --- |
| Named exports | Name offset, then a position | 8 bytes |
| Indexed exports | A position | 4 bytes |
| Export tree | Test bit, left child, right child, export index | 8 bytes |

Source: `ExportNamedSymbolEntry`, `ExportIndexedSymbolEntry` and `ExportTreeEntry` in `cro_helper.h`.

A name offset is an offset into the file, not a distance into the string table, although it lands inside it. The loader rebases it by the module address in `RebaseExportNamedSymbolTable` along with every other offset in the header, which is what says so.

An indexed export has no name. What identifies it is where it sits in the table, which is the index an importing module asks for.

The export tree is a bit-indexed trie over the names, there to make lookup fast on the console. Hexlith reads the array and ignores the tree, because a tool that reads every entry once gains nothing from an index.

## CRR

A `.crr` file is a hash container. The four hashes at the front of each CRO are themselves hashed, and the result is checked against the CRR's hash table, which is how the console verifies a module before running it.

Hexlith does not verify modules. The CRR is noted here so that its presence in the RomFS is not mistaken for a module.

## Observed in the reference image

133 `.cro` files, `static.crs` and `.crr/static.crr`, all in the RomFS root. The 134 CRO-family modules total 7,651,328 bytes on disk.

Summed across segment types, **over the 133 loadable modules and not over the static module**, whose segments belong to the main executable and are counted with it:

| Segment type | 133 modules | With `static.crs` |
| --- | --- | --- |
| Code | 4,342,512 | 8,782,284 |
| ROData | 1,157,492 | 1,590,700 |
| Data | 186,008 | 950,192 |
| BSS | 493,632 | 493,632 |

The code column with the static module is the whole lift, which is the figure [ADR 0004](../adr/0004-cro-modules-and-the-ldr-ro-hle.md) reasons about. BSS is the same in both columns because the static module declares no BSS segment.

Against the static module's 4,439,772 byte text segment, that puts 49.4 percent of the title's executable code in dynamically loaded modules, and the whole lift at 8,782,284 bytes of ARM, which is about 2.2 million instructions.

Relocation and import totals across all 134 modules:

| Table | Total entries |
| --- | --- |
| Internal relocations | 44,151 |
| External relocations | 21,868 |
| Anonymous imports | 18,991 |

The largest modules by code segment size are `DllField.cro` at 970,532 bytes and `DllBattle.cro` at 840,188. `DllField.cro` alone holds 10,237 internal relocations, 3,951 external relocations and 2,976 anonymous imports, against only two named exports, which shows the shape of the problem: a module exports almost nothing and imports almost everything.

`static.crs` exports 461 named symbols with a 17,559 byte string table, imports from 78 modules, and declares 328 anonymous imports and 331 external relocations.

Many modules are developer test stubs, named after individuals, each 8,192 bytes with a single export and no imports. They are presumably unreachable in a retail build and are low-priority targets for the lifter.

Every module declares six segment table entries, with the ones it does not use left zeroed. `DllField.cro` uses four of them and `static.crs` five. A parser that stopped at the first empty entry would therefore still read the right segments, and one that trusted the count has to tolerate empty entries among the real ones.

`static.crs` declares no internal relocations, which follows from what it is: it describes code that is already at its final addresses, so there is nothing in it to relocate.

`DllField.cro` imports from 30 modules and carries a 60 byte export string table, against the 78 modules and 17,559 bytes of the static module. The comparison is the shape of the problem in one line: the main executable publishes names and the modules consume them.

### Exports, read

640 named exports across the 134 modules: 461 from `static.crs` and 179 from the 133 loadable modules, which is between one and two each. Not one module declares an indexed export, so the whole export mechanism in this title is by name.

The named exports resolve to positions in three of the four segment types, and the split between the static module and the loadable ones is sharp:

| | Code, multiple of four | Code, odd | ROData | Data |
| --- | --- | --- | --- | --- |
| `static.crs`, 461 | 3 | 338 | 112 | 8 |
| 133 modules, 179 | 179 | 0 | 0 | 0 |

No code export sits at an even address that is not a multiple of four, in either column.

**An odd position in a code segment is a Thumb entry point.** The low bit is the interworking flag that `BX` reads, not part of the address, and the pattern in the table is exactly what that convention predicts: no code export is at an even address that is not a multiple of four, because a Thumb function pointer always has the bit set and an ARM one never does.

Checked against the bytes rather than taken on the pattern alone. The `.code` image at the four addresses below, with the low bit cleared:

| Export | Address | First bytes | Instruction |
| --- | --- | --- | --- |
| `_ZNSt14__rw_exceptionC1Ev` | 0x00100784 | `10 b5` | `push {r4, lr}`, Thumb |
| `_ZNSs11_C_allocateEjjj` | 0x001012fe | `7f b5` | `push {r0-r6, lr}`, Thumb |
| `_ZNSsaSEPKc` | 0x00101ccc | `38 b5` | `push {r3, r4, r5, lr}`, Thumb |
| `nnroAeabiAtexit_` | 0x004d5df0 | `38 40 2d e9` | `push {r3, r4, r5, lr}`, ARM |

So the export tables settle the instruction set of 520 addresses without any analysis, 338 of them Thumb. Three of the static module's 341 code exports are ARM, which says the main executable is built for Thumb almost throughout, as a handheld title written for code density would be.

Every one of the 179 module exports is ARM, which is the opposite and is worth not over-reading. 179 entry points across 133 modules say what the loader calls into, not what the module is made of: an ARM entry point can be a veneer in front of Thumb code. What the modules are compiled for is a question for the decoder, not for this table.

Parity means nothing outside a code segment. The 16 odd ROData exports are all `_ZTS` type name strings, which are character arrays and sit wherever the linker put them.

One open question: `static.crs` declares a fifth segment of 0x1F4 ROData bytes at virtual address 0x04000000, which does not correspond to any extended header code set. It is recorded here as unexplained rather than guessed at. Its three real segments sit at 0x00100000, 0x0053C000 and 0x005A6000, matching the extended header's code set addresses exactly, which is what cross-checks both readings.
