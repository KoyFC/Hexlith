# 0021. The Vulkan backend is held to the reference within a measured tolerance

Status: Accepted
Date: 2026-10-23

## Context

The reference renderer agrees with Azahar's software renderer to the bit ([ADR 0019](0019-the-reference-rasterizer-follows-azahar-and-samples-two-ways.md)), and the Vulkan backend is held to the reference. Bit for bit is not possible there. Two things a GPU does its own way cannot be changed from a shader:

- **Blending.** Core Vulkan blends in fixed function. For an eight-bit buffer it computes the source and destination terms in a precision of its own and rounds the result to nearest; the reference, after Azahar, computes `(s·f + d·f) / 255` in integers and truncates. A blended pixel can come out one step higher, and the title blends every draw, stacking additive particles ten deep in places.
- **Depth interpolation.** The GPU interpolates depth in its own precision. A depth exactly on the boundary between two integers of the buffer's format can land on either side.

Everything else the backend can make the same: the fragment shader is the reference's fragment stage in GLSL, computing in the same integers and floats; textures are decoded by the reference's code and sampled by its formulas; and vertices are placed on the PICA's grid of sixteenths of a pixel before Vulkan sees them, so that coverage follows the same fill rule.

## Decision

The backend's pictures are compared with the reference's, in the reference's filtering mode, screen by screen, by `scripts/compare_screens.py`. A picture is within tolerance when:

- the mean difference over every channel of every pixel is at most **1.0**;
- the 99.9th percentile of the differences is at most **12**;
- at most **0.01%** of the pixels differ by more than 12 in any channel.

These come from the reference frames of [`docs/gpu-reference.md`](../gpu-reference.md), measured on the Intel GPU and on llvmpipe, which gave the same numbers to within 0.003:

| Screen | Mean | 99.9th percentile | Beyond 12 | Largest |
| --- | --- | --- | --- | --- |
| Top, F1 220 | 0.63 | 2 | 0 | 2 |
| Bottom, F1 220 | 0.86 | 2 | 0 | 2 |
| Top, F2 400 | 0.70 | 9 | 0 | 11 |
| Top, 700 | 0.13 | 2 | 0.003% (4 pixels) | 123 |
| Top, 1000 | 0.67 | 4 | 0 | 5 |
| Top, F3 1100 | 0.69 | 4 | 0 | 5 |
| Bottom, 1000 and 1100 | 0.00 | 0 | 0 | 1 |

That blending is what the first two numbers measure was checked: with the reference changed for the experiment to round its blend to nearest, the differences on frames 220 and 400 fell to at most 2, on under 2% of the pixels. A mean of 1.0 allows the one step most blended pixels take; 12 allows the deepest stacks of additive particles, which reach 9 to 11. The few pixels beyond 12 are single pixels of one-pixel particles at frame 700, a coverage difference on triangles too small for snapping to settle.

## Consequences

The comparison catches a backend that draws the wrong thing: drawing with the cull modes swapped puts the title screen's top screen 97.7 on average from the reference's, and the X logo 1.9 with 6% of its pixels beyond 12.

It does not catch an error of one step. Dividing by 256 in the combiners' modulate instead of 255, or leaving out the alpha test, which in the title only ever drops fragments that blending would have left invisible, stay within the tolerance. What covers those is the synthetic test `VulkanGpuBackend.DrawsWhatTheReferenceDraws`, which draws without blending and holds the colour buffer to the reference's to the byte and depth to one unit, and the shader's being a line-by-line port of a stage that is itself held to Azahar to the bit.

The numbers are for the reference title's frames. A title that blends more deeply, or tests depth for equality, may need them looked at again, with the measurements this ADR records as the precedent.
