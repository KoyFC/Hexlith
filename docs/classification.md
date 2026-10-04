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

## What is not done yet

**The 41 percent the walk does not reach.** Some of it is code reached through a dispatch the analysis cannot follow, some is data, and nothing here can say which without more work. The honest form of that is a gap in the tables.

**Jump tables.** A computed branch through a table ends a path. The tables are not found, the entries are not followed, and no region is marked as one. This is the largest single gap and the hardest case, which is why [ADR 0006](adr/0006-analysis-layer-format.md) gave `jump-table` a region kind of its own before anything could produce one.

**The nine conflicts.** Each is a place where two paths disagree about an encoding, and each is worth reading rather than counting.

**The CRO modules.** The walk runs on the main executable only. A module's own code is reached through its relocation tables, which are not read yet.
