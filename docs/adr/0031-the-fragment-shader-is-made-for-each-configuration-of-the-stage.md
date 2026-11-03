# 0031. The fragment shader is made for each configuration of the fragment stage

Status: Accepted. Amends [ADR 0022](0022-the-vulkan-backend-keeps-gpu-memory-true.md), which left shaders specialised for a draw for when speed mattered more than one path that is easy to compare.
Date: 2026-11-03

## Context

[ADR 0022](0022-the-vulkan-backend-keeps-gpu-memory-true.md) draws every fragment with one shader that reads the stage's configuration from the draw's registers: six combiner stages, three texture units, lighting, fog. [`docs/gpu-speed.md`](../gpu-speed.md) measured waiting for the device at 5.1 s of the 10.8 s a window adds to a boot to frame 1101 on the Intel GPU, and with every draw left out of the command buffers a run to frame 700 waited a quarter as long.

What decides whether a shader for each configuration pays is how many configurations there are and what each costs to make. Both were measured. The 28,340 draws of the boot to frame 1101 have 67 distinct configurations, counting the combiners' sources, operands, operations and scales, the textures' formats and modes, fog, lighting and the alpha test, and leaving out what the stage only reads as numbers.

## Decision

**A draw's fragment shader is the generic one with the registers that configure the stage written in as constants,** one for each configuration met, made the first time a draw has it. `reg(id)` in the specialised source is a `switch` on the id that returns the value for each register of the configuration and reads the draw's words for every other, so a call with a constant folds, and a call inside the combiners' loop folds where the driver unrolls the loop; the gain measured below is what it did, not a promise about what it does on another driver. The pipeline's key carries the number of the shader, so one configuration drawn with two blend states is two pipelines of one shader.

**The generic shader stays,** as `FragmentShading::Generic`, and `hexlith-boot --generic-fragments` selects it. What the specialised shader draws is held to the generic one's to the byte by `VulkanGpuBackend.DrawsWhatTheGenericFragmentShaderDrawsWhenSpecialised`, over combiner operations, sources, scales and the alpha test, and the generic shader is itself held to the reference by ADR 0021. The live pictures of frames 220, 400, 700, 1000 and 1100 are the same as with the generic shader to the last digit of `compare_screens.py`'s figures.

## Why not the alternatives

**A table of all 1,024 registers, indexed at run time.** The first form tried. The compiler cannot fold an indexed constant array, the shader stays as large as before, and the Intel driver took 7.4 s to compile each one, 520 s for the boot.

**Specialisation constants.** Vulkan's own mechanism, which a driver folds as it does constants. A constant is a scalar of the pipeline and the stage has 59 registers that configure it, each an entry in every pipeline's creation: no source to generate, and nothing in the shader that says a loop over them can be unrolled. The generated source is the same code the generic shader is, which is what the test relies on.

## Consequences

The first run on a machine pays for compiling the shaders: 70 pipelines took 72 s on the Intel GPU with Mesa's cache empty, about a second each, and 0.02 s with the cache warm, which Mesa keeps on disk. That is a cost of the first boot, not of the next, and a cache the backend keeps itself would not make the first any shorter. What the backend does is compile the GLSL of each new configuration on another thread, as soon as a draw has it, and wait for it only when the pipeline is made, which saved about 0.3 s of the 1.3 s that compiling 67 shaders takes on the thread that runs the game. Making the pipelines ahead of the draw as well, drawing with the generic shader until they are ready, is what would make the first boot as quick as the next, at the price of which shader drew a frame depending on timing.

The specialised shader changes what the device waits for, 5.3 s to 3.7 s with a warm cache in a single run, and `docs/gpu-speed.md` has the measurement with the rest of the phase.

A configuration the shader's constants do not cover is drawn correctly, since every register not named is read from the draw as before. What the configuration leaves out on purpose, the combiners' constant colours, the textures' addresses and sizes and the lighting tables' data, changes from draw to draw and is never specialised for.
