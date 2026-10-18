# 0018. A PICA shader interpreter is the semantic reference, and it calculates in float32

Status: Accepted
Date: 2026-10-18

## Context

The PICA200 runs a vertex shader for every vertex and, when a title asks, a geometry shader for every primitive. The programs are the title's own, uploaded in command lists: four vertex programs and one geometry program before the title screen settles, 24 different instructions among them. The Vulkan backend has to translate them, and a translation is only as good as what it is checked against.

The hardware's arithmetic is float24: sixteen bits of mantissa and seven of exponent, with reciprocals and square roots that are approximations. Nobody has published what the approximations are, and this project has no console to measure one on. Azahar, the only oracle to hand, calculates in float32 and keeps a handful of behaviours its authors measured on hardware: a multiplication of zero by infinity gives zero, MAX and MIN pick a side when one operand is a NaN, the call stack misbehaves when four calls end on one instruction, an address register outside the range of a signed byte counts as zero.

## Options

**No interpreter; translate straight to SPIR-V** and compare pictures. A wrong picture would then say nothing about which instruction was wrong, and a translation can only be compared with another translation.

**An interpreter in float24**, rounding every result to sixteen bits of mantissa and flushing denormals, which is closer to the hardware as GBATEK describes the format. It cannot be checked against anything: Azahar would disagree with it in the last bits everywhere, and the hardware's own rounding and its approximations are unknown, so the float24 interpreter would be a guess that cannot be told from a bug.

**An interpreter in float32 with the measured behaviours**, which can be checked against Azahar instruction by instruction, and which the translation can be checked against in turn.

## Decision

The third, in the spirit of [ADR 0003](0003-interpreter-as-the-semantic-reference.md). `domain::gpu::RunShader` interprets vertex and geometry programs in float32, with these rules:

- Inputs, uniforms and fixed attributes are float24 or float32 values converted without loss (ADR 0017).
- A product whose result is a NaN when neither operand was one is zero: MUL, MAD, MADI and every term of DP3, DP4, DPH and DPHI. A dot product sums from a positive zero, term by term, in order.
- MAX gives the first operand when it is greater and the second otherwise, and MIN the first when it is less, so that a NaN in the first place loses and one in the second wins.
- RCP, RSQ, EX2, LG2 and FLR are the host's division, square root, exp2, log2 and floor, which are IEEE float32 operations or the C library's.
- MOVA truncates towards zero, and a NaN or a value outside the range of a 32-bit integer becomes the most negative integer, as an x86 conversion does. An address register outside -128 to 127 adds nothing; an address that lands past c95 reads ones.
- Control flow follows GBATEK's four stacks, with Azahar's measured details: a full stack drops its oldest entry, four calls ending at once leave the program counter where the fourth would have taken it from, and a jump on the last instruction of a scope is not taken.
- CMP's operators 6 and 7 always hold, as GBATEK observed; Azahar leaves the flag as it was.
- DST, DSTI and LITP are not implemented. GBATEK gives formulas it is unsure of, Azahar does nothing for them, and the reference title does not use them. A program that reaches one is reported as having done so.

`ShaderPrecision::Float24` rounds every arithmetic result to float24 as GBATEK's format describes, to nearest even with denormals flushed to zero. It is there to measure how far the hardware's precision could move a vertex, not to draw with.

## Consequences

The interpreter can be compared with Azahar's one instruction at a time, and is, by `scripts/compare_pica_shaders.py`. The GLSL the Vulkan backend generates is held to the interpreter in turn.

Where the hardware's float24 arithmetic and its approximate reciprocals differ from float32, Hexlith differs from the console the way Azahar does. Rounding to float24 moves a vertex of the title's own programs by 0.0035 pixels at most, against a rasterizer that resolves sixteenths; `docs/pica-shaders.md` records the measurement. The approximate reciprocals are not part of that, since nobody has published them.

When a hardware oracle exists, the float24 rules move into the primitives here and nothing above them changes.
