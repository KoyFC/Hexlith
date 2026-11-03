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

One jump is resolved from the image. A switch is whatever jump table region the analysis found eight bytes after the load, as [ADR 0023](adr/0023-switches-and-tables-of-functions-are-recognised-by-shape.md) describes.

A jump through a literal is not resolved, although it looks as if it could be. The main executable has 234 import stubs, each a load of the program counter from the word after it, and it is tempting to read that word as the constant it is in the file and call the stub a branch. The first run of lifted code showed it is not: `ldr:ro` writes the addresses of a module's functions into those words when the module is loaded, so a stub that went to a resolver at the start of the run goes to module code afterwards. The main executable's text is mapped read-only for the game, and the loader still rewrites its literals. `FlowContext::literals_are_constant` says whether a caller knows better, and nothing in the real pipeline does. A stub is an indirect jump, and the lifted code loads the word each time.

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
| Indirect jump | 4,170 |
| Tail call | 1,742 |
| System call | 914 |
| Switch | 518, with 5,680 cases |
| Trap | 17 |

**Indirect calls** are 13,406 blocks, 16 percent of the calls, and are a virtual call or a call through a function pointer. The lifted code needs a way from a guest address to a function that is cheap, because it is on the path of every one of them.

**Indirect jumps** are 4,170. 324 are import stubs, as above. Of the rest all but 5 are `bx` through a register that is not the link register: a virtual call made as a tail call, or the return of a function that kept its return address somewhere else. The 5 are computed adds of the program counter. They leave the lifted code and go through the dispatcher or the interpreter.

**The 17 traps** are in 17 functions, and several are the addresses the analysis reports as instruction set conflicts. They are the places where two paths disagree about what a byte is.

The survey takes about 0.6 seconds for the whole executable.

## The intermediate representation

[ADR 0024](adr/0024-the-lifter-lowers-instruction-by-instruction-and-the-interpreter-takes-the-rest.md) has the reasoning. In short, `Lower` turns one decoded instruction into an `ir::Instruction`: its condition, and a list of operations in `src/domain/lift/ir.h`.

An operation reads or writes processor state (a register, a flag, the instruction set bit, a thread ID register), computes on temporaries, calls one of the shared primitives (`Shift`, `ComputeAlu`, `ConditionPassed`), loads or stores, or leaves for the interpreter. Temporaries do not outlive their instruction. The registers an instruction reads are read at the point it needs them, so the lowering is a faithful copy of the order the interpreter reads and writes in, which matters when a load faults after some of its work and when a branch reads its target before it writes the link register.

`Evaluate` in `evaluate.h` runs those operations against a `CpuState` and a memory. It is how a lowering is checked: `tests/domain/lift/differential` runs an instruction through the interpreter and through its lowering from the same state and compares every register, every flag, the instruction set, the thread IDs and the writable pages. For an instruction that does not complete, the interpreter stops with the state unchanged, and the lowering has to bail with the state unchanged or hand the instruction to the interpreter and stop with the same reason.

What is translated so far: data processing with the shifter, MUL and MLA, single loads and stores of every width and addressing mode, block loads and stores in the four addressing modes (and so PUSH and POP), the sign and zero extensions, B, BL, BLX and BX, the hints, the thread ID registers of CP15 and the c7 barriers. Everything else is one `Interpret` operation. Those classes are checked in `tests/domain/lift/lower_test.cpp`, with the encodings checked against another disassembler.

A block transfer that faults part of the way is the case worth stating. The interpreter writes the words before the fault and stops, and the lowering bails at that instruction. The test harness then has the interpreter execute the instruction from the state the bail left, and requires it to stop with the same reason and leave the same memory. That holds because a store writes the same values to the same addresses when it is done again.

## Checking the lowering at scale

`tests/domain/lift/random_test.cpp` decodes random ARM words and random Thumb halfwords, gives each a random state (registers that point into the data page about half the time, random flags, random thread IDs), and compares the interpreter with the lowering. `HEXLITH_LIFT_RANDOM_CASES` sets how many of each; the default is 4,000 and runs in a few seconds. On 2026-11-02, with 1,500,000 of each, in a release build, the lowering and the interpreter agreed on every one: about 130 seconds for the ARM words and 150 for the Thumb halfwords.

## How much of the executable is translated

`hexlith-lift --survey` lowers every distinct instruction the functions hold and counts those that go to the interpreter. With the table of a run it weighs them by how often the game executes them, which is the figure that predicts speed. For the boot to the title screen, 41.9 million instructions:

| | Distinct instructions | Executed |
| --- | --- | --- |
| Data processing, single transfers, multiplies, branches | translated | |
| Before block transfers and extensions | 11.1 percent interpreted | 15.42 percent interpreted |
| After | 6.2 percent interpreted | 4.61 percent interpreted |

What is left, by how much it runs, is floating point (`vldr`, `vstr`, `vmul`, `vmov`, `vcvt`, `vldm`, `vmla` and the rest of VFP), the exclusive pair `ldrex` and `strex`, the doubleword transfers, the long multiplies and the supervisor call. Floating point is the largest part of it.

## The generated code

`src/domain/lift/emit` writes one function as C++, and `runtime.h` is what that C++ includes. [ADR 0025](adr/0025-lifted-code-answers-where-the-guest-continues.md) has the protocol: a function answers the address the guest continues at, can be entered at any block, and charges the clock a block at a time.

Each function has a header comment with its address, instruction set and block count, a switch that jumps to the block an address names, and then a label for each block. A block opens with the check that the slice has enough ticks left and the charge, then has a braced group for each instruction with its address and mnemonic in a comment, then its exit. What a group does is a call to the primitive the interpreter calls, on the state in `cpu`. The functions that are not translated are one call that has the interpreter execute the instruction.

`Engine` in `engine.h` is `arm::Run` with the faster path. Given the same state and limit it stops for the same reason, leaves the same state and counts the same number of instructions, and a test checks that for generated sample functions at every limit from one instruction to sixty-nine. The sample functions are built from machine code the test writes, lifted and emitted by a program the build runs (`sample_generator`), and compiled into the test: a loop, a call and a return, a switch through a jump table, loads and stores, block transfers, conditional execution, a thread ID read and an instruction the lifter leaves to the interpreter.

## Building and running it

```sh
hexlith-lift --work-dir ~/recomp-data/work --emit
scripts/build-lifted.sh ~/recomp-data/work/<title>/lift
hexlith-boot --work-dir ~/recomp-data/work --rom-dir ~/recomp-data/rom --new-game \
    --lifted ~/recomp-data/work/<title>/lift/build/libhexlith_lifted.so
```

[ADR 0026](adr/0026-lifted-code-is-built-in-the-workspace-and-loaded-as-a-library.md) has why it is shaped like this. For the main executable the output is 22,489 functions in 273 source files, 11.7 million lines and 469 MB, and a cold build takes about eight minutes on eight cores and 54 minutes of processor time. The library is 90 MB.

## Lifting the modules

```sh
hexlith-lift --work-dir ~/recomp-data/work --emit-modules
scripts/build-lifted.sh ~/recomp-data/work/<title>/lift/modules
```

The modules are a library of their own, `lift/modules/build/libhexlith_modules.so`, so that a change to the lifting of one does not rebuild the other. [ADR 0029](adr/0029-cro-modules-are-lifted-against-a-base-the-loader-gives.md) has the design: a module's code is lifted at offsets from its start, the file offsets its analysis tables use, and runs against a base the engine puts in the context.

Each module is a namespace, `lifted_modules::<name>`, because two modules have a function at the same offset, and has shards of its own, `<name>_NNNN.cpp`. A module is read as the file holds it, and that is not what the processor runs, because the loader has patched every word a relocation writes. The lifter is told which words of a module's code those are, and an instruction on one is left to the interpreter, which reads the word from memory. They are not all words the analysis calls data: 146 of them it calls code, and the count that showed it is in the ADR.

`registry.cpp` hands the loader, for each module, its name, the SHA-256 of its code segment as the file has it, and a table of entries keyed by offset. `manifest.txt` says the same in text, beside the lifter's version. The digest is how a module whose code is not the code that was lifted is found and left to the interpreter.

`hexlith-boot` and `hexlith-lift-verify` open the library of modules when `--lifted` is given and it is built beside the main one, and say so on a `modules` line. A library that is not built is not an error, the modules run interpreted, and one from another lifter is, because running without it would pass for lifted. The engine hears the loader's load and unload events, and finds a module's code in the code segments of the modules that are loaded now, at whatever address the game put them. A module whose code does not hash to what was lifted, or that has no lifted code, runs interpreted, and the run says which on its standard error when the module is loaded.

## First results

On 2026-11-02, from a new game to the first frame after the title appears, with the input script `100 tap a`, `200 tap a`: 41,972,790 instructions.

- **The golden trace of the lifted run is byte for byte the interpreter's.** So are the instruction count and the tick count, 1,017,217,878, which is the clock the kernel's timed events run on. Lifted code enters the clock a block at a time and the run agrees with the interpreter's to the tick.
- 99.4 percent of the instructions ran as lifted code. The 0.6 percent that did not, 251,885, are the interpreter's: the floating point instructions, the exclusive pair, the supervisor calls, and the code of the language selection module, which is not lifted.
- Time spent running the game's code, as `hexlith-boot` now reports it, apart from the system calls and services: the interpreter 2.7 to 3.1 seconds, 14 to 16 million instructions a second; lifted code 0.6 to 0.7 seconds, 58 to 69 million. About four and a half times faster, with the registers still in memory, every guest memory access a call into the binary, and no instruction translated beyond what ADR 0024 lists. The boot as a whole is about three times faster, because a great deal of its time is the GPU memory fills and the start-up.

A bug the run found is in ADR 0026: the import stubs of the main executable are rewritten by the module loader.

## Verifying it against the interpreter

`hexlith-lift-verify` boots the title twice from a new game with the same input script, once through the interpreter and once through the lifted library, and runs the two side by side. After every step of instructions, 100,000 by default, it compares every thread's registers, flags, floating point registers, exclusive monitor and thread ID registers, and every mapped page of memory. Lifted code is meant to leave exactly what the interpreter leaves after any number of instructions, so the first step after which the two differ holds a mistake.

That step is then narrowed down by booting both again and stopping them halfway through what is left, until one instruction separates the last count at which they agreed from the first at which they did not. The tool prints that count, where the thread was when they last agreed, and the first register or byte that differs. Lifted code runs a block only when the slice has all of the block's instructions left, so the instruction found is the last of the block that went wrong, and the address printed is where the block was entered or before.

The search is `application::VerifyLiftedCode`, which takes a function that starts a run, so the tests drive it with small synthetic kernels and a lifted function written by hand to be wrong.

```sh
hexlith-lift-verify --rom-dir ~/recomp-data/rom --work-dir ~/recomp-data/work --input <script> \
    --lifted ~/recomp-data/work/<title>/lift/build/libhexlith_lifted.so [--limit N] [--step N]
```

On 2026-11-02 the boot to frame 227, 42 million instructions, agreed throughout, compared every 100,000 instructions, in 18 seconds. So did the first 500 million, to about frame 400, compared every million, in 55 seconds.

## Measuring the speed

The phase's gate compares the time the interpreter and lifted code take to run the game's own code. A boot to the title spends less than half a second of that time with lifted code, so a single run of each says little: a core changing clock or another program waking up moves it by a tenth. `scripts/measure_lift_speed.py` makes the comparison the same way every time:

- Both builds boot from a new game to frame 227 with the input script `100 tap a`, `200 tap a`, which is where the title screen's modules have loaded and almost everything that ran is the main executable's code. Past that frame most of what runs belongs to the modules, which this phase does not lift.
- The interpreter and lifted code are run alternately, seven times each by default, so that a slow drift in the machine falls on both.
- Every run is pinned to one core, by default the last of the processor's performance cores, and the result names it.
- Before each run the script waits until the one-minute load average falls below a threshold, 1.0 by default, and prints the load it started at.
- Every run must execute the same number of instructions, or the script stops: a run that did different work measures nothing.
- What is compared is the time `hexlith-boot` reports on its `guest` line, to the millisecond, and the result is the median of each build and the ratio of the two medians.

```sh
scripts/measure_lift_speed.py --boot build/release/tools/hexlith-boot/hexlith-boot \
    --lifted ~/recomp-data/work/<title>/lift/build/libhexlith_lifted.so \
    --rom-dir ~/recomp-data/rom --work-dir ~/recomp-data/work --input <script>
```

On 2026-11-02 the machine was a laptop with four performance and four efficiency cores, the `powersave` governor and a desktop session whose load average never fell below 1.5, so the measurements ran with `--max-load 2.0`. Three measurements gave:

| Core | Runs | Interpreter, median | Lifted, median | Ratio |
| --- | --- | --- | --- | --- |
| 7, efficiency, times to the hundredth | 7 | 2.060 s | 0.430 s | 4.79 |
| 7, efficiency | 7 | 2.035 s | 0.392 s | 5.19 |
| 3, performance | 11 | 2.422 s | 0.494 s | 4.90 |

Lifted runs spread by about 15 percent from fastest to slowest, the interpreter's by less. The ratio sits on the gate, between 4.8 and 5.2, and this machine cannot place it more finely than that. The instruction count was 41,972,790 in every run.

After the analysis took in what a run executed ([`classification.md`](classification.md#what-a-run-executed-as-a-fact)), the lifted library covers 594 more functions, and the instructions the boot to frame 227 leaves to the interpreter fall from 251,885 to 196,063. Measured the same way, later the same day, with the lifted runs now spreading by a few percent:

| Core | Runs | Interpreter, median | Lifted, median | Ratio |
| --- | --- | --- | --- | --- |
| 3, performance | 7 | 1.614 s | 0.305 s | 5.29 |
| 3, performance | 11 | 1.640 s | 0.321 s | 5.11 |
| 7, efficiency | 7 | 1.434 s | 0.297 s | 4.83 |

The interpreter, which the library does not touch, ran a third faster than in the morning's measurements, so the machine was in another state of clock and power, and only the ratios are comparable. On a performance core lifted code is now five times faster than the interpreter, by a margin of a few percent; on an efficiency core it is not.

A boot to frame 1101 takes 3.87 billion instructions, and `hexlith-boot` stops at two billion unless given a limit, so a measurement to that frame passes `--limit 20000000000` to the script. Without it the script measured the first two billion instructions of the boot and reported 4.80, which is not the frame's work.

## What the interpreter's handlers do

Weighed by a run of two billion instructions, what lifted code still left to the interpreter was 7.7 percent of what the main executable executed, and two thirds of that was the exclusive pair `ldrex` and `strex`; in the boot to frame 227 it was 4.6 percent, most of it floating point. Each went through `StepDecoded`: an out-of-line call that checks the condition and the operands, dispatches on the operation and moves r15 on, with the instruction decoded at run time the first time the site ran.

Floating point, the exclusive pair, the doubleword transfers, the long and halfword multiplies and the count of leading zeros now lower to one operation, `Execute`: the interpreter's handler for the instruction does its work, called through `arm::ExecuteHandler`. The condition is checked as for any lowered instruction, the operands were checked when it was lowered, and an instruction of these families that writes r15 still goes to the interpreter whole. The generated code writes the decoded instruction out as a `constexpr` literal of `arm::Instruction`, field by field, so the dispatch on the operation is resolved by the compiler. The evaluator runs the same function, so the random differential covers it: 500,000 ARM words and 500,000 Thumb halfwords agreed with the interpreter, as did the boot to frame 227 under `hexlith-lift-verify`.

The gain is small, about four percent of the lifted time: 0.321 to 0.308 s for the boot to frame 227, a ratio of 5.11 to 5.32 on a performance core over eleven runs each. At `-O1` the compiler inlines the dispatch but not the family handlers behind it, `ExecuteFloatingPoint`, `ExecuteTransfer` and the rest, so the registers an instruction names are still read from the instruction at run time. Inlining those as well is the next step for these instructions, and it has to be weighed against the size and the build time of 65,000 sites. The library builds in under six minutes on eight cores.

## Where Phase 8 ended

The phase lifted the main executable and stopped there; the modules are Phase 9. Measured on 2026-11-02, from a new game with the input script `100 tap a`, `200 tap a`:

| | |
| --- | --- |
| Boot to frame 227 | 41,972,790 instructions, the same count and the same 1,017,217,878 ticks as the interpreter |
| Its time in the game's own code | 0.305 s lifted against 1.6 s interpreted, 5.1 to 5.3 times faster on a performance core over eleven runs, 4.8 on an efficiency core |
| Left to the interpreter outside lifted code | 0.47 percent |
| Agreement with the interpreter | every thread and every mapped page, compared every 100,000 instructions to frame 227 and every million to 500 million |
| Executed instructions within a function the analysis knows | 100.00 percent of 729.5 million, over two billion instructions |
| Boot to frame 1101 | 3.87 billion instructions, 104 s, of which 64 percent interpreted, nearly all of it module code |
| Building the library | 279 shards, about six minutes on eight cores with ccache, eight to nine cold |
| The title's screens in a window to frame 227 | 12.6 s on the Intel GPU, 23 s on llvmpipe |

The machine ran faster in the afternoon than in the morning, the interpreter alone by a third, so times from different parts of the day are not comparable with each other and the ratios are.

What is not done, and why:

- **Registers and flags in local variables.** Lifted code reads and writes every register in `CpuState`, and a store to guest memory is a store through a pointer to bytes, which the compiler has to assume may change `CpuState`, so it reads the registers again after each one. It is the largest lever left, and a change to every instruction the emitter writes and to every place lifted code leaves, so it was not started once the gate was met.
- **The handlers behind the dispatch.** `Execute` calls them out of line at `-O1`; inlining them is the next step for floating point and the exclusive pair, to be weighed against the size of 65,000 call sites.
- **The fifteen conflicts nothing executes.** Their verdicts are in [`classification.md`](classification.md#the-conflicts-read); one region and a few function records still hold the losing side, at no cost at run time.
- **The modules**, which are Phase 9 and [ADR 0029](adr/0029-cro-modules-are-lifted-against-a-base-the-loader-gives.md). The count it asked for found 146 words in module code that a relocation writes and the analysis calls code, and so the lifter will leave any instruction at a relocated word to the interpreter.


## Where Phase 9 ended

The phase lifted the 133 CRO modules, in a library of their own, and the title now runs lifted from a new game to frame 1101. Measured on 2026-11-03, with the input script `100 tap a`, `200 tap a`:

| | |
| --- | --- |
| Boot to frame 1101 | 3,867,002,588 instructions and 4,933,730,742 ticks, the same as the interpreter's, and a golden trace of 21,056 lines that is the interpreter's byte for byte |
| Left to the interpreter | 124,505,915 instructions, 3.22 percent, where Phase 8 left 64 percent |
| Entries into lifted code | 82,781,467, of which 30,659,832 into modules |
| Agreement with the interpreter | every thread and every mapped page, compared every 100,000 instructions to 200 million and every 10 million to frame 1101 |
| Time, same afternoon | 33 s lifted against 145 s interpreted for the boot to frame 1101 |
| Speed ratio, on a performance core | 4.58 to frame 1101 (143.6 s against 31.3 s, medians of three runs each); 5.27 to frame 227 over eleven runs, where Phase 8 measured 5.1 to 5.3 |
| The modules | 44,032 functions in 381 shards, 12.8 million lines and 536 MB of C++, written in 39 s; a library of 144 MB that builds cold in 7 minutes 19 seconds on eight cores |
| Words of module code a relocation writes | 38,344, whose instructions are left to the interpreter |

Most of the title's first 1101 frames is the title screen's own loop: 2.45 billion of the 3.87 billion instructions are in `DllTitle`, and 1.42 billion in the main executable.

What the interpreter still runs is mostly code the analysis has not found. 118 million instructions of `DllTitle`, 4.81 percent of what ran in it, are in no function the analysis knows, in 21 runs, and two of them, one of 631 instructions and one of 484, are 93 million of the 118. `hexlith-boot --record-executed` writes what a run executed in each module by offset ([`analysis-tables.md`](analysis-tables.md#what-a-run-observed)) and `scripts/executed_coverage.py --modules` measures it, but the analysis of the modules does not read those tables yet, as the analysis of the main executable does ([`classification.md`](classification.md#what-a-run-executed-as-a-fact)). Reading them would have the next lift cover those runs.

What is not done:

- **Binding an import to the lifted function it resolves to** when the loader links it, the optimisation of [ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md). A call from one module to another still goes through the pointer the loader wrote and the engine's lookup.
- **The analysis taking in what a run executed in a module.**
- **Registers and flags in local variables**, and the handlers behind the dispatch, as at the end of Phase 8.

## Where Phase 10 ended

The phase made the window cheaper and the interpreter's share smaller; it did not reach real time. Measured on 2026-11-03 with the input script `100 tap a`, `200 tap a`:

| | |
| --- | --- |
| Boot to frame 1101 | 3,867,002,588 instructions and 4,933,730,742 ticks, as before, and a golden trace that is the interpreter's byte for byte |
| Left to the interpreter | 11,885,820 instructions, 0.31 percent, where Phase 9 left 3.22 |
| Agreement with the interpreter | every thread and every mapped page, compared every 10 million instructions to frame 1101 |
| Time without a window | 27.6 s median, where it was 32.6 |
| Time with the window on the Intel GPU | 43.4 s median, where it was 59.9; 1.5 times the console's 18.4 s |
| Library of the main executable | 23,276 functions, 120 MB; the modules' 144 MB; both regenerated and rebuilt in 54 minutes |

[`gpu-speed.md`](gpu-speed.md) has where the window's time goes and what is left. [ADR 0030](adr/0030-the-vulkan-backend-keeps-render-targets-resident.md) is the decision that removed most of it. What is not done:

- **Shaders specialised for a draw's configuration**, the largest cost left in the window, 6.0 s of waiting for the device.
- **The colour conversions and display transfers on the GPU**, 3.4 s.
- **Registers and flags in local variables**, the lever for the game's own code, which runs at 2.3 times real time.
- **Binding an import to the lifted function it resolves to**, which Phase 10 estimated at under a second.
- **The oracle comparisons of the PICA** (`scripts/compare_pica_*.py`) were not rerun, since they need a patched build of Azahar this machine does not have built. The phase added to `src/domain/gpu` only the memory ranges of commands, the range a screen shows and the texture cache, and the rasterizer, the command processor and the pixel formats are untouched; `scripts/compare_screens.py` held ten pictures from the replay to ADR 0021's tolerance on both devices, byte for byte those of the previous design.

