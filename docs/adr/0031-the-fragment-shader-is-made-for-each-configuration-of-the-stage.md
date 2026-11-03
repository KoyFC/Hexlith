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

**A draw never waits for a shader.** The first version waited, and a person playing found the game standing still for a second or more at each new screen after the language prompt: the Intel driver compiles a shader when the pipeline is made, about a second each with Mesa's cache empty, and measured over frames 200 to 420 with the cache off there were pauses of 1.2, 3.4, 6.5 and 7.6 s, 43.7 s of pipelines in all. A configuration's GLSL is now compiled on another thread from the first draw that has it, and its pipeline is made on another thread when that draw needs it; until it is ready, the draw takes the pipeline of the generic shader for the same state. Both draw the same pixels, to the byte, so what a frame shows does not depend on how long a compile took, and a frame may be drawn with either. The same frames with the cache off now pause 0.58 s at most, and the boot with it warm drew 262 of 28,340 draws with the generic shader.

A person who reaches a screen no run has been to pays for it once on each machine, in compile time that no longer stops the game; a cache the backend kept itself would not make that shorter, and Mesa keeps the driver's.

The specialised shader changes what the device waits for, 5.1 s to 3.3 s, and `docs/gpu-speed.md` has the measurement with the rest of the phase.

A configuration the shader's constants do not cover is drawn correctly, since every register not named is read from the draw as before. What the configuration leaves out on purpose, the combiners' constant colours, the textures' addresses and sizes and the lighting tables' data, changes from draw to draw and is never specialised for.
