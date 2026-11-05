# The window's speed

Phase 10 asks how close the window comes to the speed of the same run without one. The question is asked of stretches of a boot, not of the whole of it: the first 227 frames are almost all drawing, and what follows the language prompt is mostly the game's own code.

## How it is measured

`scripts/measure_window_speed.py` boots from a new game with the input script `100 tap a`, `200 tap a` to frame 1101, through the lifted library, and has `hexlith-boot` print the wall time and the game's time at frames 227, 400, 700, 1000 and 1101 (`--time-frames`). Configurations are run alternately, three times each, on the four performance cores (the Vulkan driver and llvmpipe have threads of their own, so a single core would measure a machine nobody draws on), waiting for the load average to fall first. SDL's dummy video driver is used, so the desktop's part of presenting is left out. Every run must execute the same instructions by each frame, or the script stops. What is reported is the median of each stretch.

After a run with a window, `hexlith-boot` prints where the display's time went: the lists, split into texture decoding, converting the colour and depth buffers into the backend's images and back, the wait for the device and making buffers; the display transfers; and composing and presenting the screens. Whatever remains of the lists is the command processor and the vertices the CPU shades.

The pictures of the live path are checked the same way the replay's are. `hexlith-boot --window --save-frames 220,400,700,1000,1100 --save-frames-to <directory>` writes both screens of those frames as `frame-<n>-top.png` and `frame-<n>-bottom.png`, once with `--backend reference` and once with `--backend vulkan`, and `scripts/compare_screens.py` holds the second directory to the first within the tolerance of [ADR 0021](adr/0021-the-vulkan-backend-is-held-to-the-reference-within-a-measured-tolerance.md). A frame in which the game drew nothing is not handed to the display, and the picture of the next one stands for it, so a frame is saved one picture after it and one after the last picture is never saved. On 2026-11-03 all ten pictures of the five frames were within the tolerance on the Intel GPU, with means between 0.00 and 0.86, the same as the replay gave.

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

## Phase 11

Measured on 2026-11-03 with a real window on the Wayland desktop, medians of five alternating runs on the four performance cores, load average 1.6 to 2.0. The machine ran the game faster than in the afternoon of Phase 10, its own 27.6 s without a window being 17.4 s now, so only the ratios of one session compare. The stretch from frame 227 to 1101 is what the milestone is about: the console takes 14.6 s, the run without a window 17.4 s.

| 227 to 1101 | Seconds | Over the floor |
| --- | --- | --- |
| Before the phase (3 runs) | 24.85 | +46% |
| Generic fragment shader, after the CPU changes | 25.0 | +44% |
| Fragment shader for each configuration | 23.85 | +37% |
| Its pipeline made on another thread, the draw takes the generic one meanwhile | 23.07 (floor 18.0 that session) | +28% |
| The milestone, 15% | 20.0 | +15% |

What each change took off the boot to frame 1101 on the Intel GPU, seconds in the display's own accounting:

| | Before | Now |
| --- | --- | --- |
| Display transfers | 1.13 | 0.23 |
| Colour and depth back to memory | 0.76 | 0.70 |
| Waiting for the device | 5.07 | 3.27 |
| Lists, whole | 6.28 | 5.30 |

- **67 fragment configurations** in 28,340 draws, by [ADR 0031](adr/0031-the-fragment-shader-is-made-for-each-configuration-of-the-stage.md)'s count of the registers that configure the stage. Making a shader for each is within reach; compiling them costs 1.3 s of glslang on every run, and about a second each in the driver the first time on a machine, 72 s in all, which Mesa then keeps.
- **The display transfers** are one transfer, RGBA8 in tiles to RGB8 in rows, 1,321 times, and a tile at a time they take 0.17 ms instead of 0.86. Converting the colour of a target a tile at a time took only 0.1 s off writing it back: that cost is moving a megabyte, not converting it.
- **What is left of the 8.0 s the window adds to the stretch** is waiting for the device, 3.3 s, of which 1,909 submissions at about 0.55 ms are 1.0 s and the draws the rest; composing and presenting, 0.9 s; the CPU work around the lists that is neither of those, about 1.3 s; moving targets up and down, 1.1 s; and what the GPU driver's own threads take from the game's cores, which nothing here measures.
- **Every one of those is time the game's thread spends waiting or working for the display.** A window within 15 percent of the floor needs the display's work to overlap the game's code rather than follow it, which means a list submitted without waiting for it and a display transfer the GPU can do. Both change what ADR 0030 says about when a group waits, and are not small.

The pictures of frames 220, 400, 700, 1000 and 1100, saved from the live run with `--save-frames`, are within the tolerance of ADR 0021 as before, and `compare_pica_textures.py` and `compare_pica_rasterizer.py` agree with Azahar on 2,000 textures, 3,500 transfers and steps and the recording to frame 400 after the transfer changes.

Making a configuration's pipeline on another thread, and drawing with the generic shader's until it is ready, also takes the compile out of the lists, 5.30 s to 4.53 s, and is what keeps the game from standing still at a new screen: with Mesa's cache off, frames 200 to 420 paused up to 7.6 s while the pipelines were made on the game's thread and pause 0.58 s at most now. The last measurement was made on 2026-11-03 in the same way, medians of five runs, a real window, the floor at 18.0 s that session for the same stretch; the window takes 23.07 s, 28 percent over, where the milestone is 15.

## Phase 12

Phase 12 asks the window to overlap the game's code instead of following it: lists submitted without waiting for them, and the display transfer done on the device. What it does with each is recorded below as it is done. The milestone is a ratio, measured in one session: from frame 227 to 1101 the window adds no more than 15 percent to the same stretch without one, which `scripts/measure_window_speed.py --configs none,intel --runs 5` reports with a real window on the Intel GPU.

### How the device's own time is measured

`--gpu-time` sets `INTEL_MEASURE=batch` for the runs that draw with Vulkan. Mesa's Intel driver then writes one line for each command buffer it submits, with the time the device spent on it between its own timestamps, and the script adds those up. What the host waited for beyond that is latency: submitting, the device starting, and the host noticing it has finished. Mesa now and then leaves a short line of three fields among the rows, which the script skips, and in a run with a real window its file held 3,153 rows of the 3,229 submissions. The driver's own bookkeeping costs a little: a boot with it took 42.9 s where one without took 42.1.

### Baseline

Measured on 2026-11-05 with the binary of the end of Phase 11, a real window on the Wayland desktop, five alternating runs on the four performance cores, load average 0.93 to 0.99 before each. Every run executed 3,867,002,588 instructions. The machine ran the game slower than in Phase 11's session, 27.9 s without a window for a stretch that took 18.0 s then, so only the ratios of one session compare.

| 227 to 1101 | Seconds | Over the floor |
| --- | --- | --- |
| Console | 14.61 | |
| No window | 27.89 | |
| Intel, window | 35.01 | +26% |
| The milestone, 15% | 32.07 | +15% |

The window adds 7.12 s to the stretch, and the milestone allows 4.18, so the phase has to take 2.94 s off it.

Where the display's time went in the boot to frame 1101, with the window: lists 6.26 s (textures 0.23, up 0.62, down 1.16, waiting for the device 3.99, buffers 0.57), transfers 0.41 s, present 1.39 s. 1,909 groups of draws, 1,383 MB up and 692 MB down, no conflicts.

**The device worked at least 1.49 s of the 3.99 s the host waited.** The backend made 3,229 submissions, 1,909 groups of draws and 1,320 writebacks, and Mesa's file held 3,153 rows of them in these runs, so the 1.49 s is a floor and the other 2.50 s at most latency, at most 0.77 ms for each submission. A diagnostic run on 2026-11-05 with SDL's dummy video driver measured the device at 2.53 s of 4.37 s waited, over 3,229 rows, all of them: how long the device takes varies from run to run, with the clocks the driver chooses, and the latency does not. A writeback is made for each display transfer, and the CPU converts its colour. Half of the wait is not drawing and is not reduced by drawing faster, only by waiting for fewer submissions or not waiting.

**What the title's display transfers are.** In the recording of the boot to frame 1150, 1,369 display transfers, and 1,321 of them to frame 1101. All but one are the same: RGBA8 in tiles to RGB8 in rows, flags `0x1004`, no flip and no scale, an input of 256 by 512 and an output of 240 by 400 (684 of them) or 240 by 320 (684). The one that differs is in frame 0, 128 by 128 from RGBA4 in tiles to RGBA4 in rows, flags `0x4400`. The bytes written back to memory in a boot to frame 1101 are 1,320 colour buffers of 256 by 512 by 4, so every writeback is the colour a display transfer reads.

### Where Phase 12 ended

Measured on 2026-11-05 the same way as the baseline above, an hour later: the binary of the end of the phase, a real window on the Wayland desktop, five alternating runs on the four performance cores, every run executing 3,867,002,588 instructions. The first two runs started with the load average at 2.5 and 2.1, the desktop's own, and the rest between 0.92 and 1.00.

| 227 to 1101 | No window | Intel, window | Over the floor |
| --- | --- | --- | --- |
| Baseline of the phase | 27.89 s | 35.01 s | +26% |
| End of the phase | 27.39 s | 30.20 s | +10% |
| The milestone | | | +15% |

**The milestone was met.** The window adds 2.81 s to the stretch where it added 7.12 s.

Where the display's time went in the boot to frame 1101, with the window, seconds in the display's own accounting:

| | Baseline | Now |
| --- | --- | --- |
| Lists, whole | 6.26 | 1.90 |
| Waiting for the device | 3.99 | 0.28 |
| Colour and depth back to memory | 1.16 | 0.13 |
| Colour and depth into the images | 0.62 | 0.39 |
| Making buffers | 0.57 | 0.01 |
| Textures decoded | 0.23 | 0.21 |
| Display transfers | 0.41 | 0.08 |
| Composing and presenting the screens | 1.39 | 1.18 |

The device worked at least 1.35 s over 3,150 of its submissions (Mesa's file holds a few fewer rows than the backend made, as before) and the host waited 0.28 s, so most of its work now runs alongside the game's code. A single boot reports 221 waits for the device in all, ring, writebacks and transfer outputs together; they were not told apart.

- **Making buffers, 0.57 s to 0.01 s,** is the commit that keeps them from group to group: 10,865 buffers made in a boot became 109.
- **Submitting without waiting and carrying out the transfer on the device** are [ADR 0032](adr/0032-groups-of-draws-are-submitted-without-waiting.md) and [ADR 0033](adr/0033-the-display-transfer-runs-on-the-device-and-its-output-is-written-when-read.md). Neither was measured alone with the protocol of five alternating runs. Single boots to frame 1101 with SDL's dummy video driver, which draws the window in software, took 42.1 s before the phase's changes, 39.3 s with the buffers kept and the groups submitted without waiting, and 34.3 s with the transfers on the device too, so the order of what each is worth is in those numbers and no more than that.
- **1,319 of the 1,321 display transfers** to frame 1101 are done on the device. The title's transfers read the last rows of a taller target, 112 rows down in a target of 512 for the top screen and 192 for the bottom's 320, which the first tests of the backend did not cover: it declined every one of them, with 1,320 waits, until a test that starts below the top of the target was written.

What is left of the 2.81 s the window adds to the stretch: the command processor and the vertices the CPU shades, which are the 0.88 s of the 1.90 s of lists that is left once uploads, textures, writing back, the wait and buffers are taken out; composing and presenting, 1.18 s; and what the driver's own threads take from the game's cores, which nothing here measures. Presenting from the device, and the CPU's vertex work moving there, are what would reduce them. The stretch with the window is 1.10 times the one without, and 2.07 times the console's 14.61 s, where the one without is 1.87 times: the game's own code is what stands between this and real time, not the window.

The pictures of frames 220, 400, 700, 1000 and 1100, saved from the live run with `--save-frames` with the backend submitting without waiting and carrying out the transfers on the device, are within the tolerance of ADR 0021 as before, the same figures to the last digit as with the changes of Phase 11. The golden trace of the boot is the one of the same boot without a window, byte for byte, and the boot to frame 400 with the Khronos validation layer loaded gives it no message. `src/domain/gpu` is as Phase 11 left it, so the comparisons with Azahar's renderer were not run again.
