# 0023. Switches and tables of functions are recognised by shape, and the interpreter is the fallback for the rest

Status: Accepted
Date: 2026-11-01

## Context

The lifter translates the code the analysis found. On the reference image the walk of Phase 4 reached 80 percent of the text segment, and a run of the title from a new game to the title screen showed what that number hides: 6.5 percent of the instructions the game executed were in no function extent. Three causes account for nearly all of it.

A switch compiled to a table ends the path. The load that dispatches through the table is a computed branch, the table is not recognised, its entries are not followed, and the functions that contain switches are measured as ending at the table. Ghidra resolved 450 such jumps with 3,227 targets, of which Hexlith had not reached 3,019.

A tail call to a function nothing else calls was a branch, not a call, so what it reached was never named as a function. Such a function has no caller to record it.

A virtual call goes through a table of function addresses in read-only data, and the functions in it are often leaves, with no push or stack adjustment for the prologue test to find. The sweep of data dropped them.

Whatever the analysis misses, the interpreter can run. ADR 0003 makes it the permanent fallback, so a gap costs speed and does not cost correctness. That is what lets these be recognised by heuristics and not proofs, as long as a wrong recognition cannot make the lifted code wrong.

## Decision

**A switch is recognised by its shape.** A word load into r15 from the program counter plus a register shifted left by two is a table dispatch. The table starts eight bytes after the load, which is what the program counter reads, and its entries are absolute addresses. The count comes from the compare just before the load when there is one: `cmp rm, #n` with `ls` has n + 1 entries and with `lo` has n. Without a compare the table runs while the words are word aligned addresses in the code range, and has to hold two. A table whose bound is not met, or that overlaps bytes already decoded as instructions, is refused and counted, and nothing is claimed.

An accepted table is a `jump-table` region, which is what ADR 0006 made that kind for. No fifth table is added: the region says where the table is and how big, the dispatch is the instruction eight bytes before it, and the entries are the words in it. Its entries are code reached by this path, so they are walked at the confidence of the path and are not named as functions.

**An unconditional branch to a push is a tail call.** A prologue runs once, on the way in, so a branch to one is never a jump inside a function. The target is recorded as a function.

**A run of code addresses in data is a table of functions.** Three or more consecutive words that look like ARM code addresses are taken without the prologue test. Three conditions keep that from taking case labels and integers, all measured: the sweep now runs after everything stated and everything followed from it has been walked, and a word that points into code already walked is left alone, because that is where a case label points and a function does not. Words with the interworking bit set are left to the prologue test, since Thumb is rare in this title and a Thumb address among plain words is more often an integer. Measured on the reference image, the rule with none of this produced 947 instruction set conflicts against the 33 there were, the order and the walked-code check brought that to 535, and leaving Thumb words to the prologue test brought it to 36.

**A function's extent follows its own regions.** A literal pool or a switch table splits a function's code into several regions, so the measurement follows a branch into any code in the function's instruction set within the unbroken run of regions from its own, with data and tables in between, and follows the entries of its jump tables. A gap, bytes the walk never reached, ends the run. Without that limit the measurement chained through tail calls into millions of bytes.

**Nothing downstream may treat these as proofs.** An extent, a table or a function boundary is a hint. A lifter that reaches a transfer it cannot resolve exits to the interpreter with exact state, and the analysis is checked by running the title rather than by agreeing with itself.

## Why not the alternatives

**Import Ghidra's jump tables.** Ghidra is an oracle and not a source of truth, and its output cannot enter the repository. It also needs several minutes against half a second here. It remains the check: after this change it resolves 450 tables and Hexlith has not reached 8 of its targets.

**A table of dispatches.** It would duplicate what the region and the instruction already say, and every table the analysis writes is one more to keep in step with a hand edit. If a dispatch ever needs a fact the region cannot carry, such as a table in another section, that is the day to add one.

**Prove each function pointer.** There is no proof to be had for a word in data. The prologue test was the proof's stand-in and it was too strict for leaves.

## Consequences

Measured on the reference image, from a new game to the first frame after the title appears, 41,947,687 instructions in the main executable:

| | Before | After |
| --- | --- | --- |
| Text segment reached | 80.3 percent | 92.2 percent |
| Functions | 17,309 | 22,489 |
| Executed instructions in a function extent | 93.53 percent | 99.84 percent |
| Executed instructions in a code region | 96.99 percent | 99.87 percent |
| Jump tables | 0 | 501 |
| Targets of Ghidra's jump tables not reached | 3,019 of 3,227 | 8 |
| Code only Ghidra found | 547,174 bytes | 37,816 bytes |
| Instruction set conflicts | 33 | 36 |
| Overlapping functions | 8 | 54 |

Hexlith now calls 8,382 entries functions that Ghidra does not, of which Ghidra has 4,879 as code and 3,503 as not. The run executed 481 of them. The rest are unverified: they are guesses of the confidence the walk always gave guesses, and the interpreter remains the answer to a wrong one.

The overlaps rose because a function that reaches a case shared with another is measured to include it. A lifter works from an entry and a control flow graph, not from an extent, so an overlap duplicates code and does not change what runs.

The CRO modules gain 227 functions from tail calls and no jump tables: their table entries are written by relocation, and the analysis reads the file before it. That is for the phase that lifts them.

Tables dispatched by a computed `add pc` or by a load from data followed by `bx` are not recognised. There are two of the first in the main executable, and they stay with the interpreter.
