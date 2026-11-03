# The window's speed

Phase 10 asks how close the window comes to the speed of the same run without one. The question is asked of stretches of a boot, not of the whole of it: the first 227 frames are almost all drawing, and what follows the language prompt is mostly the game's own code.

## How it is measured

`scripts/measure_window_speed.py` boots from a new game with the input script `100 tap a`, `200 tap a` to frame 1101, through the lifted library, and has `hexlith-boot` print the wall time and the game's time at frames 227, 400, 700, 1000 and 1101 (`--time-frames`). Configurations are run alternately, three times each, on the four performance cores (the Vulkan driver and llvmpipe have threads of their own, so a single core would measure a machine nobody draws on), waiting for the load average to fall first. SDL's dummy video driver is used, so the desktop's part of presenting is left out. Every run must execute the same instructions by each frame, or the script stops. What is reported is the median of each stretch.

After a run with a window, `hexlith-boot` prints where the display's time went: the lists, split into texture decoding, converting the colour and depth buffers into the backend's images and back, the wait for the device and making buffers; the display transfers; and composing and presenting the screens. Whatever remains of the lists is the command processor and the vertices the CPU shades.

The floor is the same boot without a window. A stretch whose game time alone is longer than the console takes for it is held up by the CPU, not by drawing.

## Baseline

Measured on 2026-11-03 before any change of the phase, on the laptop of Phase 8 and 9 (Intel Lunar Lake GPU, llvmpipe), medians of three runs, load average between 1.2 and 2.1. The same afternoon the whole boot took 32.6 s without a window.

| Frames | Console | No window | Intel | llvmpipe | Game alone |
| --- | --- | --- | --- | --- | --- |
| 0 to 227 | 3.79 s | 0.50 s | 10.33 s | 16.16 s | 0.31 s |
| 227 to 400 | 2.89 s | 5.53 s | 8.39 s (+52%) | 10.39 s (+88%) | 5.35 s |
| 400 to 700 | 5.01 s | 10.91 s | 16.49 s (+51%) | 21.43 s (+96%) | 10.57 s |
| 700 to 1000 | 5.01 s | 11.39 s | 18.11 s (+59%) | 22.82 s (+100%) | 11.04 s |
| 1000 to 1101 | 1.69 s | 4.31 s | 6.59 s (+53%) | 7.88 s (+83%) | 4.20 s |
| Whole boot | 18.40 s | 32.6 s | 59.9 s | 78.5 s | |

Where the 27 s that Intel adds went (the whole boot):

| | Intel | llvmpipe |
| --- | --- | --- |
| Lists | 20.8 s | 40.3 s |
| of which textures decoded | 3.4 s | 3.3 s |
| of which colour and depth into images | 4.1 s | 4.0 s |
| of which colour and depth back to memory | 4.1 s | 3.5 s |
| of which waiting for the device | 7.1 s | 24.7 s |
| of which making buffers | 0.5 s | 0.1 s |
| Display transfers | 2.0 s | 1.7 s |
| Composing and presenting the screens | 3.3 s | 2.8 s |

There are 1,909 groups of draws, each moving 2,002 MB up and 2,002 MB down in all, about a megabyte each way per group.

## What it says

- **The window costs about half again as much as the run does, on every stretch after 227,** and the stretches are alike, so the cost goes with the work and no one scene is to blame.
- **Making buffers, which the plan expected to matter, does not:** 0.5 s of 60. Keeping them between lists is not worth a commit of its own.
- **The round trip through memory is what costs on Intel:** converting up and down, 8.2 s, and the wait for the device, 7.1 s. Keeping the targets resident removes most of the three.
- **Three costs are not the round trip** and stay after it: decoding textures, 3.4 s; composing the screens, 3.3 s, done once a frame whether or not anyone looks; and the transfers, 2.0 s.
- **The game alone is 2.3 times the console's time** from frame 227 on, so no change to the window makes that stretch real time.

## The milestone

N is set from these numbers. On Intel, from frame 227 to 1101, the window adds no more than 15 percent to the same stretch without a window, and the boot to frame 1101 takes at most 34 s with the window, once the CPU work of the phase has taken about 3.5 s off the floor. Frames, validation messages, the golden trace and the instruction count are held as before.

## Where Phase 10 ended

Measured on 2026-11-03 the same way, with a real window on the Wayland desktop instead of SDL's dummy driver, which draws the window in software and costs about 5 ms a frame to present where the real one costs 1.8 ms. Medians of three runs, load average 1.4 to 2.4:

| Frames | Console | No window | Intel | llvmpipe | Game alone |
| --- | --- | --- | --- | --- | --- |
| 227 to 400 | 2.89 s | 4.88 s | 6.37 s (+30%) | 9.13 s (+87%) | 4.70 s |
| 400 to 700 | 5.01 s | 9.64 s | 12.89 s (+34%) | 19.10 s (+98%) | 9.29 s |
| 700 to 1000 | 5.01 s | 9.49 s | 13.26 s (+40%) | 18.75 s (+98%) | 9.14 s |
| 1000 to 1101 | 1.69 s | 3.37 s | 4.55 s (+35%) | 6.20 s (+84%) | 3.25 s |
| Whole boot | 18.40 s | 27.6 s | 43.4 s | 67.1 s | |

The Intel runs were 43.4, 43.9 and 28.9 s. The third is not explained and is a third of the runs; the medians do not depend on it.

| Boot to frame 1101, with the window on Intel | Baseline | Now |
| --- | --- | --- |
| Whole boot | 59.9 s | 43.4 s |
| Lists | 20.8 s | 8.3 s |
| Colour and depth into the images | 4.1 s | 0.6 s |
| Colour and depth back to memory | 4.1 s | 1.4 s |
| Waiting for the device | 7.1 s | 6.0 s |
| Textures decoded | 3.4 s | 0.25 s |
| Making buffers | 0.5 s | 0.6 s |
| Display transfers | 2.0 s | 2.0 s |
| Composing and presenting the screens | 3.3 s | 1.05 s |

The no-window boot fell from 32.6 to 27.6 s with the analysis of the modules taking in what a run executed ([`classification.md`](classification.md#in-the-modules)).

**The milestone was not met.** The window adds 30 to 40 percent to the stretches after frame 227 and the boot to frame 1101 takes 43.4 s, where the aim was a 15 percent addition and 34 s. What is left of the 15.8 s the window adds on Intel:

- **Waiting for the device, 6.0 s, is drawing.** With every draw left out of the command buffers a run to frame 700 spent 1.2 s in the wait where it spent 4.7 s with them. A group costs 0.55 ms to submit and wait for and a draw about 0.15 ms to run, on a fragment shader that reads the registers of the draw and goes through six combiner stages and three texture units for every fragment. Overlapping the wait with the game's code does not help, because the display transfer that follows each list needs the colour at once. Shaders specialised for the draw's configuration are what would reduce it; [ADR 0022](adr/0022-the-vulkan-backend-keeps-gpu-memory-true.md) left them for when speed matters more than one path that is easy to compare.
- **Writing the colour back, 1.4 s, and the display transfers, 2.0 s**, are the conversions between the PICA's tiled formats and the images', done by the CPU a pixel at a time. The transfer is the colour buffer into the framebuffer a screen shows, which the GPU could do.
- **The game's own code, 2.3 times the console's time** from frame 227 on, is not the window's. Registers in local variables is what would move it.

What the backend gets right without extra cost: no run had a conflict, a write nobody announced to a dirty target.
