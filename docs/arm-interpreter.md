# The ARM interpreter

[ADR 0003](adr/0003-interpreter-as-the-semantic-reference.md) made an interpreter the executable reference for what the ARM11 does, written before the lifter so that the semantics are settled before any of them are baked into generated code. This note says what it implements, which choices of the architecture it makes and why, and how it was checked.

## Shape

`Step` executes the instruction at r15 and `Run` repeats it up to a limit, and can say how many instructions completed, which is what the kernel's clock counts; a supervisor call completes, since r15 has moved past it. Both live in `src/domain/arm/interpreter`, and both are templates over the `GuestMemory` concept, compiled in `interpreter.cpp` for `PagedMemory` and for the page table the kernel runs the game on.

Every operation goes through the shared primitive library in `src/domain/arm/primitives`: the shifter and addition with carry, the multiplies, saturation, the parallel instructions, packing, and floating point. Generated code will call the same functions, so the interpreter and the lifter can only disagree by calling different ones.

The processor is in user mode and stays there. The game never leaves it and the kernel is reimplemented rather than run, so there are no banked registers, no saved status registers and no exception entry. The state is the sixteen general registers, the user flags, Q, GE and T, the floating point registers and FPSCR, the local exclusive monitor, and the two CP15 thread ID registers user mode can reach.

An instruction the interpreter cannot execute exactly is a stop, with a reason, and the state is left as it was before it. The reasons are a supervisor call, a breakpoint, an undefined word, an unpredictable use of a defined instruction, a privileged instruction, an unsupported one, a memory fault and an alignment fault. A supervisor call is the exception: it moves r15 past itself first, so that the kernel can return to the next instruction.

## Choices the ARM11 makes

Where the architecture leaves something to the implementation, or where ARMv6 and ARMv7 differ, the interpreter follows ARMv6 and records the choice here.

**A data-processing write to r15 is a branch, not an exchange.** ARMv7 made `mov pc, rX` in ARM state interwork like `bx`. ARMv6 does not, and an ARM target that is not word aligned is unpredictable. A load into r15, by `ldr` or `ldm` or a Thumb `pop`, does interwork, as it has since ARMv5T.

**Unaligned word and halfword transfers happen.** ARMv6 supports unaligned access when the U bit of the system control register is set, and then `ldr`, `str`, `ldrh` and `strh` work at any address, while block, doubleword, swap, exclusive and floating point transfers fault unless word aligned. Source: the ARMv6 reference manual (DDI 0100I), section A2.8. The interpreter assumes the U bit is set. Unicorn agrees about the unaligned loads and stores it compared. The alignment faults are not checked by anything yet, because Unicorn models none in this configuration, and they are counted rather than compared below. Code compiled for the game should never cause one.

**A store of r15 stores the instruction's address plus eight.** The architecture lets an implementation choose plus eight or plus twelve.

**A Thumb branch with link is one instruction.** In ARMv6K Thumb it is two halfwords that the processor executes separately, the first leaving half the target in the link register. Treated as one, it cannot be interrupted in the middle, which nothing here does, and a branch to its second halfword alone is not supported.

**An undefined word stops whatever its condition says.** The architecture lets an implementation skip a conditional undefined instruction whose condition fails. Stopping is the choice that cannot hide data being executed.

**The hints complete at once.** `nop`, `yield`, `wfe`, `wfi` and `sev` are no operations, which the architecture allows for all of them. In Thumb they do not exist: the 16-bit encodings belong to ARMv6T2.

## What it does not do

Each of these stops rather than computing something different.

- Big-endian data. `setend le` is a no operation and `setend be` is unsupported.
- Coprocessors other than the floating point unit, apart from the CP15 thread ID registers and the three CP15 barriers user mode can issue. The barriers complete without doing anything, because a single thread has nothing to order.
- Floating point modes outside [ADR 0010](adr/0010-floating-point-on-the-host.md): a rounding mode other than to nearest, a vector length, an enabled trap. A write of FPSCR that would select one stops at the write.
- Anything privileged: changing mode, the saved status register, returning from an exception, the floating point system registers other than FPSCR.

Two things are approximations and are not stops. The exclusive monitor is local and exact-address: a store exclusive succeeds when a load exclusive opened the monitor on the same address, and nothing models another thread clearing it. The cumulative floating point exception flags are not set, as ADR 0010 records.

## How it is verified

The same way as the decoder, and for the same reason: unit tests cover the cases someone thought of, and the mistakes that matter are in the ones nobody did. The interpreter is compared with Unicorn, which is QEMU's processor emulation as a library and has an ARM11 MPCore model with VFPv2.

```sh
python3 -m venv venv && venv/bin/pip install unicorn
cmake --build --preset ci
venv/bin/python scripts/compare_arm_interpreter.py --random 200000 --defined
venv/bin/python scripts/compare_arm_interpreter.py --random 200000 --thumb --defined
venv/bin/python scripts/compare_arm_interpreter.py --random 200000 --float --defined
venv/bin/python scripts/compare_arm_interpreter.py --image ~/recomp-data/work/<id>/exefs/.code \
    --length 0x43BEDC --count 200000
```

Each case is a random state and one instruction. The registers point into a mapped data region almost half the time and hold small numbers or anything at all the rest, the flags and FPSCR are random, and the floating point registers mix random bits with zeros, infinities, NaNs of both kinds, denormals and the ends of the range. Both sides start from the same state and the same memory and run one instruction, and every general register, the user flags, FPSCR and every byte of the mapped memory are compared afterwards. `hexlith-arm-run` executes the cases on the Hexlith side.

`--defined` keeps only the words the decoder accepts, so that a run spends its cases on instructions. `--float` draws them from the floating point space. `--image` draws them from the reference image instead of at random, at every word or, with `--thumb`, every halfword.

Unicorn is a development dependency and nothing else: the build does not use it and the tests do not need it.

Current result:

| | Compared | Disagreements | Both fault |
| --- | --- | --- | --- |
| ARM, random words | 81,858 | 0 | 17,646 |
| ARM, words the decoder accepts | 99,842 | 0 | 21,674 |
| ARM, text segment | 169,921 | 0 | 23,383 |
| Thumb, halfwords the decoder accepts | 152,069 | 0 | 37,123 |
| Thumb, text segment | 165,403 | 0 | 8,921 |
| Floating point, words the decoder accepts | 142,298 | 0 | 14,567 |

Each row is 200,000 cases. A memory fault on one side has to be a memory fault on the other, which is the third column. The rest of each row is cases the interpreter stopped on before executing anything, which the script counts by reason and does not compare: undefined words, unpredictable register choices, privileged and unsupported instructions, supervisor calls and breakpoints, which Unicorn takes as exceptions into a privileged mode, and alignment faults.

The FPSCR comparison masks the cumulative exception flags, which ADR 0010 leaves out.

Over the ARM code the analysis reaches in the text segment, about 120,000 distinct words each run once from a random state, the interpreter stops as unpredictable on 11, every one because of the random state: a `pop` that loads an ARM address with bit 1 set into r15, a `bx` to one. Nothing in the reached code stops for what the instruction is.

## What the comparison found

Four mistakes, all with passing unit tests at the time. Three were in the decoder, and the Capstone comparison could not have shown them, because Capstone implements a later architecture and accepts the same words.

- A halfword or signed byte transfer with a register offset leaves bits 11 to 8 empty, and anything in them is unpredictable. The decoder accepted them. None of the 839 such words in the text segment is reached by the analysis.
- SMMLS has no form without an accumulator, and the decoder read an accumulator field of all ones as no accumulator rather than as unpredictable.
- The 16-bit Thumb hints were decoded, and they belong to ARMv6T2.
- The interpreter stopped on a block transfer with r15 as its base only when it wrote back. It is unpredictable either way.

Getting Unicorn to be a fair witness took four corrections of the script's own, each of which made every case after a certain point disagree, and each of which is now handled and commented there. Its stack pointer and link register are banked, so the mode has to be written first. It keeps translated code across a rewrite of the bytes at the same address. Its instruction count leaves the program counter of a branch behind. And it keeps executing in the instruction set the previous case ended in, whatever the next case's CPSR says. The script refuses to report at all if Unicorn runs past the instruction it was asked to stop after, so a fifth problem of that kind would show up as a failure, not as a column of disagreements.
