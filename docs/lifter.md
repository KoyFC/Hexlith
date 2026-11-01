# The lifter

The lifter turns the code the analysis found into C++ that the host compiler builds, as [ADR 0002](adr/0002-lifter-emits-cpp-not-llvm-ir.md) decided. [ADR 0003](adr/0003-interpreter-as-the-semantic-reference.md) makes the interpreter the reference it is checked against, and the instruction semantics that both share live in `src/domain/arm/primitives`. This note records how the lifter works as it is built, and what was measured on the way.

Nothing the lifter produces enters the repository. Generated code goes to the workspace and is built from there.

## Control flow

`src/domain/lift/control_flow` cuts one function into basic blocks. Its input is the code, the entry point and instruction set of the function, the set of every function's entry point, and the analysis regions; its output is the blocks in address order and how each one ends.

A block is entered only at its first instruction and left only at its last, and a call, a supervisor call and a return all count as leaving. That is not tidiness, it is what lets the lifted code agree with the interpreter on the clock. [ADR 0013](adr/0013-one-host-thread-and-a-virtual-clock.md) makes one instruction one tick, and timed events happen at an exact tick, so lifted code has to charge exactly the instructions the interpreter would have executed. The plan is to charge a block's instructions when it is entered. If a callee could hand the processor away in the middle of a block already paid for, the interpreter would finish the block later and count its tail twice. So a call ends its block, and the instructions after it are a block of their own, paid for when the call returns.

A block's cost is its number of instructions. An instruction whose condition fails still takes its tick, so conditional instructions in the middle of a block need no block of their own.

How a block ends:

| Exit | Meaning |
| --- | --- |
| Fallthrough | The next instruction is entered from elsewhere too |
| Jump | A branch to an address in this function, possibly conditional |
| Tail call | A branch to another function's entry point, or to bytes outside the image |
| Call, indirect call | BL and BLX to an address, and BLX through a register |
| Indirect jump | A jump whose destination the image does not settle |
| Return | BX lr, a move of lr into the program counter, a pop of it |
| Switch | A load through a table of code addresses, with the cases |
| System call | SVC |
| Trap | An instruction the interpreter has to take over at |

Every exit but a fallthrough carries the condition of its instruction. When it fails, control continues at the instruction after it, and so it does after a call or a supervisor call whatever the condition was.

**A trap is where the lifter gives up honestly.** An undefined word, a breakpoint and the end of the image become an empty block that exits to the interpreter at that address, with the state exactly as it is, and the interpreter reports why. The lifted code never has to guess.

Two jumps are resolved from the image. A switch is whatever jump table region the analysis found eight bytes after the load, as [ADR 0023](adr/0023-switches-and-tables-of-functions-are-recognised-by-shape.md) describes. A load of the program counter from the word that follows it is a branch to a constant, which the compiler uses to reach a function too far away for a branch, and it is a jump or a tail call to that constant. That is true in the main executable, whose text is mapped read-only and never relocated. It is not true in a module, where the loader writes the final address into the literal, so the builder takes a flag for it and a module's literals stay loads.

## What the main executable looks like

`hexlith-lift --survey` cuts every function in the analysis tables into blocks and counts. On the reference image, against the tables of 2026-11-01:

| | |
| --- | --- |
| Functions | 22,489, of which 472 Thumb |
| Blocks | 249,171 |
| Instructions in blocks | 1,123,626 |
| Largest function | 4,694 instructions |
| Largest block | 1,603 instructions |

The instruction total counts an instruction once for every function that reaches it, and overlapping functions and shared switch cases are counted again; it is not the size of the program.

How the blocks end:

| | Blocks |
| --- | --- |
| Jump | 105,283 |
| Call | 69,636 |
| Return | 27,969 |
| Fallthrough | 25,516 |
| Indirect call | 13,406 |
| Indirect jump | 3,846 |
| Tail call | 2,066 |
| System call | 914 |
| Switch | 518, with 5,680 cases |
| Trap | 17 |

**Indirect calls** are 13,406 blocks, 16 percent of the calls, and are a virtual call or a call through a function pointer. The lifted code needs a way from a guest address to a function that is cheap, because it is on the path of every one of them.

**Indirect jumps** are 3,846, and all but 5 are `bx` through a register that is not the link register: a virtual call made as a tail call, or the return of a function that kept its return address somewhere else. The 5 are computed adds of the program counter. They leave the lifted code and go through the dispatcher or the interpreter.

**The 17 traps** are in 17 functions, and several are the addresses the analysis reports as instruction set conflicts. They are the places where two paths disagree about what a byte is.

The survey takes about 0.6 seconds for the whole executable.
