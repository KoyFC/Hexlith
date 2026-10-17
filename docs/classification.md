# Telling code from data

Lifting needs to know which bytes are instructions, which encoding each one is in, and where each function begins. The cartridge states none of it. This note is how Hexlith works it out and what that produced on the reference image.

## Why not decode everything

The obvious approach is to decode from the start of the segment to the end and keep whatever decodes. It does not work here, and the number that says so is in [`arm-decoder.md`](arm-decoder.md): 99.5 percent of the main executable's text segment decodes as a valid ARM instruction, and the segment is full of things that are not ARM instructions. Thumb functions, literal pools inside functions, jump tables, padding. In Thumb it is worse: 89.8 percent of all 65,536 halfwords are instructions.

Whether something decodes is almost no evidence about what it is.

## The walk

So the analysis starts from addresses that are known and follows control flow.

Known means the entry point from the extended header, and the 285 function entry points the static module's export table gives up, each with its instruction set settled by the interworking bit of its exported address.

From there `WalkCode` decodes forward, instruction by instruction, carrying the instruction set along the path. At each instruction it does four things.

It records the bytes as code in the encoding it decoded them in. It follows a branch whose target it can work out, and an exchanging branch flips the encoding, which is the only way the encoding ever changes. It records the target of a call as the start of a function. It stops at anything it cannot follow: an unconditional write to the program counter, a word that is not an instruction, the end of the image.

What the walk does not reach is not called data. It is not called anything, and the tables say nothing about it. An absent region is the absence of knowledge.

## Two kinds of pointer

A branch reaches only what the instruction stream names, and this title does not reach most of its own code that way. A virtual call goes through a table in read-only data, and nothing in the instruction stream names the functions in it.

The walk therefore reads pointers, and it treats two kinds differently.

**A word a function loads from its own literal pool** is evidence from code. The instruction that loads it is real, the address it computes is real, and the four bytes it reads are data inside a function, which is worth recording on its own. If the value points into the code range at a sensible alignment it is followed.

**A word that merely sits in data and looks like an address** is a guess. Sweeping read-only data and data for those is what turns a tenth of the segment into three fifths of it, so the guess is worth making, but not without a filter.

The filter is a prologue test: the word is followed only if what it points at starts with a push, an adjustment of the stack pointer, or a store through it that writes back. Measured on the reference image:

| | Text segment reached | Functions | Instruction set conflicts |
| --- | --- | --- | --- |
| Exports and the entry point alone | 10.5 percent | 2,639 | 0 |
| With the sweep | 64.0 percent | 8,765 | 2,659 |
| With the sweep and the prologue filter | 58.9 percent | 12,126 | 9 |

A conflict is an address reached as ARM along one path and as Thumb along another. It is a contradiction, so at least one of the two paths is wrong, and it is the measure of how much the analysis is inventing.

The middle row is what an unfiltered sweep costs: five more percentage points of reach for three hundred times the contradictions. Of the 19,671 words in data that point into the code range at a plausible alignment, 5,222 point at something shaped like the start of a function, and those are the ones taken.

Everything found this way is written with `source=heuristic`, which is what lets the next run replace it and what keeps a guess from hardening into an assumed fact. [ADR 0006](adr/0006-analysis-layer-format.md) is why that field exists.

## What came out

Against the reference image, from 286 seeds and the sweep:

| | |
| --- | --- |
| Instructions decoded | 659,708 |
| Text segment reached | 2,614,274 of 4,439,772 bytes, 58.9 percent |
| ARM | 2,586,156 bytes |
| Thumb | 28,118 bytes |
| Functions found | 12,126 |
| Literal pool words | 14,803 |
| Stops at a word that is not an instruction | 6 |
| Instruction set conflicts | 9 |

**The main executable is predominantly ARM.** This corrects a reading of the export table: 338 of the static module's 341 code exports are Thumb, but the exports are the C++ runtime, which is what the CRO modules link against, and they are not a sample of the image. The Thumb in the main executable is that runtime, 28 KB of it, and everything else the walk reaches is ARM.

Six stops in 659,708 instructions is worth being suspicious of rather than pleased about. It is low because ARM decoding almost never fails, which is the same fact that makes the walk necessary, so a stop is not what ends most paths: an unconditional branch or a return is.

### Floating point comparisons ended functions early

The tables above were measured before the decoder read floating point. Until then a `vmrs apsr_nzcv, fpscr`, which copies the flags of a floating point comparison into the CPSR so that a conditional branch can test them, decoded as a coprocessor read into r15. A write to r15 is a branch, so the walk ended the function at every floating point comparison and never reached the code after it.

Decoding VFPv2 fixed that, and the same run picks up floating point literal pools, which a `vldr` relative to the program counter reads the way an `ldr` reads an integer one:

| | Before | After |
| --- | --- | --- |
| Text segment reached | 58.9 percent | 61.1 percent |
| Functions | 11,824 | 11,941 |
| Stops at a word that is not an instruction | 6 | 9 |
| Instruction set conflicts | 9 | 10 |

The other tables in this note still describe the earlier run.

### Seeds that nothing calls

Until Phase 4 the seeds were walked but not measured. The pass that measures where functions end took only the functions the walk found by following a call, so an exported function that nothing in the image calls had no record in `functions.txt`, although its code was reached and classified. 67 of the 285 exported functions were missing that way. Every seed now becomes a function record too, with the provenance of the seed:

| | Before | After |
| --- | --- | --- |
| Functions | 11,941 | 12,008 |
| Exported functions without a record | 67 | 0 |

Reach does not change, because the code was walked either way.

### Seeds from the modules' imports

The CRO modules import 4,955 distinct positions in the main executable's code, 21 of them Thumb, and each one is an entry point the loader resolves when a module is loaded. Only 1,103 of them were inside code the walk had reached. They are facts in the way the exports are, so they are seeds with `source=relocation`:

| | Before | After |
| --- | --- | --- |
| Text segment reached | 61.1 percent | 80.4 percent |
| Functions | 12,008 | 17,308 |
| Instruction set conflicts | 10 | 25 |
| Stops at a word that is not an instruction | 9 | 14 |

The conflicts rose, and they were read rather than counted. The 25 reports are 14 addresses, one of them reported eleven times. 24 of the 25 come from a path seeded by the heuristics: a call target or a literal pointer followed for its value. The one that comes from an import is at `0x00100184`, which a module imports as ARM and which a heuristic path had already walked as Thumb, so it is the heuristic path that is wrong. No position is imported both as ARM and as Thumb, and none imported as ARM is misaligned.

### Facts before guesses

That one conflict was the walk's fault and not the image's. The first path to reach a byte claims it, and a later path that disagrees stops there. The places to walk from were kept on one stack, and the sweep of data pushed its guesses after every stated seed, so they were walked first. At `0x00100184` a Thumb path from a guess got there before the import, and the function record, which takes its instruction set from the region the walk leaves, came out Thumb with `source=relocation`.

The walk now keeps three stacks and always takes from the most trusted one that has anything left: the stated seeds, then whatever is followed by branching from them, then guesses and everything followed from those. On the reference image:

| | Before | After |
| --- | --- | --- |
| Text segment reached | 80.4 percent | 80.3 percent |
| Functions | 17,308 | 17,309 |
| Instruction set conflicts | 25 at 14 addresses | 33 at 20 addresses |
| Conflicts on a path from a fact | 1 | 0 |
| Stops at a word that is not an instruction | 14 | 12 |

`0x00100184` is now an ARM function of 48 bytes ending in a `bx`. The conflicts rose because guesses that used to claim bytes first and stop a stated path now arrive second and stop themselves, and each of the 33 is a guess that was wrong or a fact that disagrees with one. None of them stops a path that started at a fact.

Without the sweep of data, the imports and the exports together reach 43.4 percent, against 10.5 for the exports alone.

## Where each function ends

The classification pass answers where functions start. The gap to the next one is not where they end: a function is followed by whatever the linker put next, which may be padding, a literal pool, or a function nothing has found yet.

So the extent is measured the same way the entry points were, by following control flow. From the entry every branch that stays inside the function is followed and the furthest instruction reached is the end. A call is not followed, because it comes back. A branch to another known entry point is not followed either, because that is a tail call and the function it reaches is its own.

An entry point the body runs through is not a function of its own. It is a second way into this one, which is what a compiler produces when it folds two functions whose endings agree, and it goes in the `alt-entries` field [ADR 0006](adr/0006-analysis-layer-format.md) put there for it.

On the reference image:

| | |
| --- | --- |
| Entry points measured | 12,126 |
| Functions | 11,824 |
| Secondary entry points folded in | 314 |
| Without a measurable extent | 0 |
| Overlapping another function | 8 |
| Bytes attributed to a function | 2,426,600 |
| Largest function | 0x190C bytes |

The size distribution is the shape compiled code has, which is the useful part of this measurement:

| Extent | Functions |
| --- | --- |
| At most 0x20 | 2,087 |
| At most 0x80 | 4,689 |
| At most 0x200 | 4,006 |
| At most 0x800 | 1,011 |
| At most 0x2000 | 31 |
| Larger | 0 |

An average of 205 bytes, nothing over 6.5 KB, and eight overlaps out of 11,824. A measurement that had run two functions together would show up as a handful of enormous extents, and a measurement that had stopped early would show up as thousands of tiny ones.

The 188 KB between the code the walk reached and the code attributed to a function is reached by falling into a region from outside any function this pass knows about, which is a gap worth closing rather than a discrepancy.

## The CRO modules

The 133 loadable modules are analysed by the same walk and the same measurement of where functions end, each in an address space of its own. Two things differ, and both come from the relocation tables.

**Where a module is.** The loader decides, so the analysis puts the module at address zero, its data where the file has it, and its BSS on the first page boundary past the end of the file. An address in a module's tables is then an offset into its file. [`cro-linking.md`](cro-linking.md) describes the linker that places the module there; the analysis links each module on its own, with nothing else loaded.

**Which words are addresses.** In the main executable a word in data that looks like an address might be one, which is why the sweep and its prologue filter exist. In a module the question does not arise. Every word that holds an address is the target of a relocation, because otherwise it would point wherever the module happened to be built for. So the walk follows no word on the strength of its value. It starts from three kinds of seed, all of them facts:

| Seed | Across the 133 modules |
| --- | --- |
| A named export in code | 179 |
| A position in code that another module, or the static module, imports | 1,006 |
| A relocated word that points into the module's own code | 27,066 |

A relocated word pointing into code is also written to the module's `pointers.txt`. 153 more relocations point at the very end of a code segment, at no instruction, and are left out. The 21,537 words that point into another module are the module's imports and say nothing about its own code.

The walk sees the code segment and nothing else, so a path that runs off its end stops there instead of decoding the read-only data after it. An earlier draft let it run on and reported 100.6 percent of the code reached.

### What came out

| | |
| --- | --- |
| Module code reached | 4,109,788 of 4,342,512 bytes, 94.6 percent |
| Functions | 43,805 |
| Instruction set conflicts | 0 |
| Stops at a word that is not an instruction | 0 |

| Module | Code | Reached | Functions |
| --- | --- | --- | --- |
| `DllField` | 970,532 | 95.9 percent | 9,216 |
| `DllBattle` | 840,188 | 96.7 percent | 6,497 |
| `DllKawaigari` | 277,436 | 97.4 percent | 913 |
| `DllBox` | 128,340 | 95.8 percent | 984 |
| `DllZukan` | 121,716 | 95.7 percent | 1,161 |

Against the main executable's 80.3 percent, with 33 conflict reports, the difference is what the relocation tables are worth: every function a module reaches only through a table of pointers is a seed here, where in the main executable it has to be guessed at. The 5.4 percent not reached includes the literal pools inside functions, which are data and not counted as code, and whatever padding sits between functions. How it splits between those and code that nothing reaches has not been measured.

20 modules carry at most 172 bytes of code each and reach about two thirds of it. Most are the developer stubs [`formats/cro0.md`](formats/cro0.md) describes, one exported function each.

Regenerating the module tables writes identical bytes, as it does for the main executable. The first attempt did not: one function in `DllBattleMatch` jumps back past its own entry into another function's code, and the bounds pass recorded that function's entry as a second way into the first, which the table reader then refused. The bounds pass now takes a second entry point only from inside the function.

`hexlith-analyze` writes a line per module to `modules/summary.txt`, with the seeds each one started from.

## Running it

```sh
hexlith-analyze --work-dir ~/recomp-data/work
```

It reads `exefs/.code`, `romfs/static.crs` and every `romfs/*.cro` out of the extraction and writes the four tables of the main executable into a directory beside it, with the four tables of each module under `modules/<name>/` in the same directory. It never touches the cartridge, so it asks for the work directory alone and not for the ROM directory.

The whole pass takes under half a second on the reference image, which matters more than it sounds: an analysis that takes an hour gets run once and trusted, and one that takes half a second gets run after every change and compared with the run before it.

Two properties hold and the tests hold them. Running it twice writes identical bytes. Editing a record by hand and running it again keeps the edit, and the derived records for that address give way to it, which is the rule [`analysis-tables.md`](analysis-tables.md) describes.

The tables are written beside the extraction and not into this repository. [ADR 0011](adr/0011-only-underivable-records-in-the-repository.md) settles what does go into `data/profiles/`: only records nothing can derive, such as a name a person worked out, and not the derived tables, which are regenerated from the image in half a second.

## Checked against Ghidra

Ghidra answers the same two questions, where functions start and which bytes are code, with heuristics of its own. It is not a source of truth any more than Hexlith is, so a disagreement between the two is a place to look and not a verdict. Where they disagree, the bytes themselves, decoded by the decoder that was checked against Capstone, are the third source.

The comparison runs Ghidra 12.1.4 headless over the decompressed `.code`, with a raw import at `0x00100000` and the language `ARM:LE:32:v6`, which is the ARM11's architecture. REA opens a raw image as ARMv7, which would accept encodings the ARM11 does not have, so the bulk comparison does not go through it. A pre-script marks the entry point, since a raw image has none, and a post-script writes function entries, runs of instructions and resolved computed jumps, as addresses and counts, into the workspace:

```sh
W=~/recomp-data/work/<id>
$GHIDRA_INSTALL_DIR/support/analyzeHeadless $W/rea/ghidra-project code \
    -import $W/exefs/.code -loader BinaryLoader -loader-baseAddr 0x100000 \
    -processor ARM:LE:32:v6 -scriptPath scripts/ghidra \
    -preScript MarkEntryPoint.java 0x00100000 \
    -postScript ExportAnalysisFacts.java $W/rea/export
hexlith-analyze --work-dir ~/recomp-data/work > analyze.txt
scripts/compare_ghidra_analysis.py --ghidra $W/rea/export --tables $W/analysis \
    --text-start 0x00100000 --text-size 0x43BEDC --summary analyze.txt
```

The analysis takes several minutes, against half a second for `hexlith-analyze`. Measured on 2026-10-16 and again on 2026-10-17 after the walk put facts before guesses, over the text segment only:

| | Hexlith | Ghidra |
| --- | --- | --- |
| Functions | 17,309 | 14,860 |
| Code bytes | 3,566,426, 80.3 percent | 3,749,360, 84.4 percent |

| | |
| --- | --- |
| Entries both found | 13,288 |
| Shared entries in different instruction sets | 0 |
| Entries only Hexlith found | 4,021, of which Ghidra has 830 as code |
| Entries only Ghidra found | 1,572, of which Hexlith has 501 as code |
| Code to both, same instruction set | 3,202,106 bytes |
| Code to both, different instruction set | 80 bytes |
| Code to Hexlith only | 364,240 bytes |
| Code to Ghidra only | 547,174 bytes |

Between them the two call 92.7 percent of the text code, so most of what each misses the other has reached. Where both call a byte code they agree on its encoding everywhere except 80 bytes. 48 of them are the function at `0x00100184`, which Ghidra has as Thumb and the import shows is ARM; before the walk was reordered Hexlith agreed with Ghidra there and the difference was 44 bytes.

**Jump tables.** Ghidra resolved 450 computed jumps with more than one target, 3,227 distinct targets in all. 371 of those jumps sit in code Hexlith already walks, and 3,019 of the targets are bytes Hexlith has not reached. That is the gap the walk leaves at every `ldr pc` through a table and every `add pc`, measured for the first time, and it is the largest one.

**The conflicts.** `hexlith-analyze` names each address it was told was both ARM and Thumb, and since the walk puts facts first, every report is a guessed path stopping. Against Ghidra, as of 2026-10-17:

| Address | Reports | Ghidra |
| --- | --- | --- |
| `0x00100048` | 1 | Thumb, a function entry |
| `0x00100184` | 1 | Thumb, inside the function at `0x00100134` |
| `0x00101C90`, `0x00101CB8` | 11 and 2 | Thumb and not code, both inside the function at `0x00101928` |
| `0x001FBFBC` to `0x0020BE5C` | 11 addresses, 12 reports | ARM at ten, not code at one, each inside a function |
| `0x0040049C`, `0x004004D8` | 2 and 1 | ARM, inside the functions at `0x004003B0` and `0x004004B8` |
| `0x004009BC`, `0x004009E8` | 1 each | not code, inside the function at `0x004008BC` |
| `0x00470048` | 1 | ARM |

Ghidra agreeing with one side does not settle a conflict, and `0x00100184` shows why. A module imports that position as ARM. Decoded as ARM, the bytes there are a complete function, a push of r4 and lr, moves, a load, an add and the matching pop, then a short tail ending in a `bx`. Decoded as Thumb, the second halfword is undefined. So the import is right, and the guessed path and Ghidra are both wrong in the same way. The table used to record the address with `isa=thumb` and `source=relocation`, which is how the order of the walk came to light. The other nineteen have not been read yet.

## What is not done yet

**The 20 percent the walk does not reach.** Some of it is code reached through a dispatch the analysis cannot follow, some is data, and nothing here can say which without more work. The honest form of that is a gap in the tables.

**Jump tables.** A computed branch through a table ends a path. The tables are not found, the entries are not followed, and no region is marked as one. Ghidra's 450, with 3,019 targets Hexlith does not reach, put a number on it. This is the largest single gap and the hardest case, which is why [ADR 0006](adr/0006-analysis-layer-format.md) gave `jump-table` a region kind of its own before anything could produce one.

**The conflicts, overlaps and stops of the main executable.** 33 conflict reports at 20 addresses, of which one has been read, and 12 stops. Each is a place where two paths disagree, about an encoding or about which function owns a byte, and each is worth reading rather than counting. The modules have no conflicts and no stops.

