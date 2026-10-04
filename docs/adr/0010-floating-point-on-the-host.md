# 0010. VFP arithmetic runs on the host's floating point

Status: Accepted
Date: 2026-10-04

## Context

The ARM11 in the 3DS has VFPv2, and the main executable's text segment holds about 55,000 floating point instructions. The primitive library of [ADR 0003](0003-interpreter-as-the-semantic-reference.md) has to give each of them a meaning that the interpreter and the generated code share.

IEEE 754 fixes the result of an addition, a subtraction, a multiplication, a division and a square root, given a rounding mode. A host and the ARM11 computing one of those in round to nearest produce the same bits. They differ elsewhere:

- Which NaN comes out. ARM prefers a signalling NaN operand over a quiet one and the first operand over the second, and an invalid operation such as infinity minus infinity produces `0x7FC00000`. x86 produces `0xFFC00000` and picks between NaN operands by its own rule.
- Flush to zero. With the FZ bit of FPSCR set, a denormal operand is read as zero and a denormal result is written as zero. The host does neither by default.
- Default NaN. With the DN bit set, every NaN result is `0x7FC00000`.
- The rounding mode in FPSCR, the short vector mode VFPv2 has and later versions dropped, and the exception traps.
- The cumulative exception flags in FPSCR, which record that an operation was inexact, overflowed, divided by zero and so on.

VFPv2's multiply-accumulate is not fused: the product is rounded, then the sum.

## Options

**A software floating point library**, such as Berkeley SoftFloat, compiled in. Exact in every mode and every flag, and roughly an order of magnitude slower than the host for each operation. The generated code would carry that cost on every floating point instruction in the game, since the generated code and the interpreter share the library.

**The host's floating point, with the edges handled explicitly.** NaN selection, FZ and DN are a few comparisons around a host operation. Whatever is not handled is refused at run time instead of computed differently.

## Decision

The host's floating point, in `src/domain/arm/primitives/vfp.h`.

Handled explicitly: NaN selection and quieting, the default NaN for invalid operations, FZ on operands and results, DN, the separate rounding of VFPv2's multiply-accumulate, and saturating conversions to integers with either truncation or ties to even.

Refused: any rounding mode other than to nearest, a nonzero vector length or stride, and any enabled trap. `IsSupportedFloatMode` answers whether an FPSCR value is in the handled set. Whatever executes an instruction checks it and stops when the answer is no.

The host compiler must not contract a multiplication and an addition into a fused operation, so the domain library is built with `-ffp-contract=off`, publicly, so that generated code inherits it.

## Why

A wrong floating point result does not crash. It moves a character a fraction of a pixel, or changes a damage calculation in a way nobody notices for hours. The decision is shaped around that: everything the library computes, it computes exactly as the hardware does, and everything it cannot compute exactly, it refuses visibly.

Speed matters because the same code runs in the generated output. Floating point in the game's field and battle code is the hot path the recompilation exists for.

## What is not exact

**The cumulative exception flags are not modelled.** No operation sets IOC, DZC, OFC, UFC, IXC or IDC. Code that reads FPSCR back sees what was last written to it. The main executable never does, as the next section says, and a comparison against an independent implementation has to mask them.

**FZ flushes the rounded result, not the exact one.** The reference manual flushes a result whose exact value is below the smallest normal number, so a value that would round up to the smallest normal number is flushed by the hardware and kept here. It needs a result within half a unit in the last place of 2⁻¹²⁶, and it is recorded rather than handled.

## What the game does with FPSCR

Measured on the main executable after the decoder learned VFPv2, by finding every VFP system register access in the text segment.

FPSCR is written in one place, at `0x00122A60`, and the value written is the constant `0x03000000`: flush to zero and default NaN set, round to nearest, no vector length, no traps. That is the mode ARM calls RunFast, and every part of it is handled. Nothing else in the main executable writes FPSCR.

FPSCR is never read into a general register. Every read, 930 of them in code the analysis reaches, is the form that copies the comparison flags into the CPSR for a conditional branch. Nothing reads the cumulative exception flags, so leaving them out costs the main executable nothing.

The CRO modules are not analysed yet and have not been measured. Until they are, the stops described above are what would show a module doing something else.

## What would change this

Two things would reopen the question. If the game is found writing a rounding mode or a vector length into FPSCR, the stop will say so, and those instructions need either a software path or the host's own rounding modes. If code is found that reads the exception flags and acts on them, the flags have to be computed, and for the inexact flag that is not cheap on the host.

## Consequences

The interpreter and generated code check FPSCR's mode when it is written, not at every floating point instruction, because FPSCR is written rarely and read on every operation.

A host whose floating point is not IEEE 754 binary32 and binary64 with round to nearest by default cannot run Hexlith. Every platform the project targets qualifies.
