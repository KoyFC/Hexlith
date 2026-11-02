# 0027. The screens are drawn as the game runs and shown in a window

Status: Accepted
Date: 2026-11-02

## Context

Until now the only way to see what the title draws was to record the GPU's work during a run and replay the recording afterwards ([ADR 0016](0016-the-gpu-recording-keeps-lists-and-changed-pages.md)). That is the right tool for comparing against Azahar, and the wrong one for seeing whether a change to the lifter or the kernel broke the game: nothing shows until the run is over and a second tool has been run on its output.

## Decision

**`LiveDisplay` is a `GpuRecorder` that draws instead of writing.** `GpuRecording` already hands a recorder every GX command, the pages of GPU memory that changed before each list, each list and each framebuffer line. `LiveDisplay` does with them what the replay does: pages are written into GPU memory, lists go to a `GpuBackend`, memory fills and display transfers run on the memory, and the framebuffer lines are remembered. A frame's picture is taken at the first event of the frame that is not a framebuffer line, the rule the replay uses, so a live run and a replay of the same events show the same pictures.

**What shows the pictures is a port, `ScreenSink`**, with one adapter, `SdlWindow`. It copies the two screens into one texture, the top over the bottom and centred as on the console, and lets SDL scale it, keeping the proportions. It also reports when the window was closed, and `hexlith-boot` stops the run at the next vertical blank when it is.

**SDL has a library of its own,** like Vulkan, so that only what opens a window links it. SDL3 is required to build the repository, `sdl3-devel` on Fedora; the tests use SDL's dummy video driver and need no display.

**`hexlith-boot --window`** takes the GPU's events in the place of `--record-gpu`, which the two cannot share because `gsp::Gpu` has one observer for each kind of event, and the command line refuses both together. `--backend reference|vulkan` and `--device` choose what draws.

## Why not the alternatives

**Present from the Vulkan backend with a swapchain.** It would be faster, since the picture would not leave the GPU, but it ties the window to one backend and the reference renderer could not be watched. The copy costs microseconds beside what drawing costs.

**Run the replay in a second thread, fed by the recording.** The game's events would need a queue and the replay a source that waits for more. It makes a window possible without touching the recording, and costs a thread, a queue and the memory it holds, for something the observers already give directly.

## Consequences

Drawing is what costs. The title reaches frame 227 in about 3 seconds of the game's own code, and in 185 s with the reference renderer, 64 s with Vulkan on llvmpipe and more on an Intel GPU, because the backend uploads and downloads GPU memory around every list (ADR 0022). A live window is therefore a way to look at the title now, slowly, and not yet to play it. Keeping the GPU's memory resident between lists is the change that would close most of that gap.

The recorder interface reads every mapped page of VRAM and the linear heap before each list to find the ones that changed. Live, that work is repeated against memory the display could read directly, which is the second thing to remove.
