# 0022. The Vulkan backend keeps GPU memory true and the PICA's fragment stage in one shader

Status: Accepted
Date: 2026-10-23

## Context

The PICA200 draws into buffers in GPU memory that the rest of the system reads: display transfers copy the colour buffer to the framebuffer a screen shows, a later list may sample what an earlier one drew, and the CPU writes vertices and textures into the same memory. A Vulkan backend has to keep that true while drawing with images of its own.

The PICA's fragment stage is configured by registers rather than programmed: six combiner stages, three texture units with their wrap modes and formats, fragment lighting with 24 tables, fog. Azahar's Vulkan renderer generates a shader for each configuration and caches it.

## Options

For memory: **cache surfaces across lists**, tracking which memory each image stands for and invalidating on writes, as Azahar's rasterizer cache does, which is fast and a large piece of code whose mistakes show as stale pictures; or **copy each list's buffers in from memory before it draws and back after**, which is slow and has nothing to get stale.

For the fragment stage: **a shader generated for each configuration**, fast on the GPU, many shaders to compile and a generator to keep in step with the reference; or **one shader that reads the configuration**, the registers themselves, slower per fragment and the same code path for every draw.

For textures: **Vulkan images and samplers**, whose wrap modes and filtering are the GPU's, or **texels in a buffer sampled by the shader**, by the reference's own formulas.

## Decision

The second of each, for now.

GPU memory stays what is true. Within a list, the backend gathers each draw's triangles, its register file and its tables; at the end of the list, and before any draw that samples a buffer a pending draw writes, it copies each target's colour and depth buffers from memory into Vulkan images, draws, and copies them back, encoded in the buffer's own formats. Memory fills, display transfers and the screens' pictures then read memory as they do for the reference.

One fragment shader serves every draw. It is the reference's fragment stage written again in GLSL, reading the draw's register file, lighting tables and fog table from a storage buffer: the same texture coordinates, wrap modes and filtering, fetched from texels the reference's own decoder wrote into a second storage buffer; the combiners in the same integers; the lighting, fog, alpha test and scissor in the same order; the depth computed as the reference computes it and rounded to the buffer's format. Blending, the logic operation, the write masks and the depth and stencil tests are Vulkan's fixed function, configured from the registers, with a pipeline for each combination met. [ADR 0021](0021-the-vulkan-backend-is-held-to-the-reference-within-a-measured-tolerance.md) says what that costs.

A draw with no geometry shader that is a list of whole triangles, with nothing left in the primitive assembler from before, has its vertices shaded on the GPU by the translation of its vertex program ([ADR 0020](0020-pica-shaders-become-glsl-compiled-by-glslang.md)), one translation for each program met. Its inputs are loaded and placed on the CPU by the reference's vertex loader, and the outputs mapped to their semantics as the reference maps them; the triangles are the vertices three at a time, which is what the PICA's assembler makes of such a draw. Every other draw, those with the geometry shader, vertices given in immediate mode, strips and fans, has its vertices shaded on the CPU by the reference's own vertex pipeline, which the backend hands its triangles. Either way, the vertex shader Vulkan runs places the vertices on the PICA's grid of sixteenths of a pixel and flips y and z into Vulkan's conventions.

A vertex the GPU shades starts from empty registers, where the reference's interpreter keeps one unit's registers for a whole draw, so that a program reading a temporary before writing it would see what the previous vertex left. The title's programs do not: its pictures are the same to the bit whichever way their vertices are shaded.

`ports::GpuBackend` takes a whole list at a time. The command processor is a template over the backend's executor, so nothing between a command word and a vertex is a virtual call (ADR 0017).

## Consequences

Nothing drawn can be stale, and the backend agrees with the reference within ADR 0021's tolerance from its first run. It is slow where a round trip costs: replaying to frame 1100 takes three minutes on the Intel GPU, which waits for each list, and forty seconds on llvmpipe, which does not need to.

The shader does more work per fragment than a specialised one would, and textures are sampled by hand rather than by the GPU's texture units. Specialising, caching surfaces, moving texture decoding to the GPU and taking strips, fans and the geometry shader there too are each an optimisation the comparison with the reference can check, to be made when speed matters more than having one path that is easy to compare.
