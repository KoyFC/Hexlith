# 0028. The live display draws in the title's own memory

Status: Accepted. Amends [ADR 0027](0027-the-screens-are-drawn-as-the-game-runs-and-shown-in-a-window.md).
Date: 2026-11-02

## Context

[ADR 0027](0027-the-screens-are-drawn-as-the-game-runs-and-shown-in-a-window.md) fed the live display through the GPU recording's interface. The display kept a GPU memory of its own, and before every command list the recording read each mapped page of VRAM and the linear heap, compared it with what it last saw and handed over the pages that differed. Live, that copy only existed to be kept in step with memory the display could have read directly.

It cost more than drawing did. Booting to frame 227 with the lifted library and the Vulkan backend on llvmpipe took 80.8 s, and about six samples in ten taken with `eu-stack` during that run were in the page comparison.

The title's VRAM and its FCRAM region are each one flat block of host memory, a `memory::Backing`, laid out the way the GPU addresses them: VRAM from its physical base, and the linear heap at the same offsets into the region as from FCRAM's physical base. `GpuMemory` is that same layout.

## Decision

**`GpuMemory` can stand over bytes it does not own,** and the live display is given one standing over the title's VRAM backing and its FCRAM region. The backend draws where the game and gsp::Gpu read and write.

**The live display takes gsp::Gpu's events itself,** through the same observers the recording uses, and reads each command list from that memory. It no longer goes through `GpuRecording`, which keeps the page comparison for the recording it makes.

**It does only what gsp::Gpu leaves undone:** command lists and display transfers. Memory fills, texture copies and DMA are already carried out by gsp::Gpu on the same memory, and doing them twice would be work for nothing. gsp::Gpu tells its observer about a command before carrying it out, so a list or a display transfer runs at the point in the queue where the console runs it.

## Why not the alternatives

**Keep the copy and find the changed pages more cheaply,** with `memcmp` against the copy and no map of shadow pages. It still reads the whole mapped heap for every list, a few milliseconds each time, and to the title there are about five thousand lists.

**Track the pages the guest writes.** It would have to be done in every store the lifted code and the interpreter make, which is the hottest path there is, to save work only a window needs.

## Consequences

Booting to frame 227 with the window takes 28.0 s instead of 80.8 s on llvmpipe. The pictures are the same: frame 220 drawn live by the reference renderer is identical, both screens, to frame 220 of a replay.

With a window, what the GPU draws and what display transfers write is in the game's memory, as it would be on the console, and without one it is not. A game that read back what it drew would behave differently with and without a window, and with one backend or the other within ADR 0021's tolerance. The title does not, as far as it has been run: the golden trace of a boot of two billion instructions, to frame 684, is the same with and without the window, and so are the instruction and tick counts.

The cost that remains is the Vulkan backend's round trip through memory for every list ([ADR 0022](0022-the-vulkan-backend-keeps-gpu-memory-true.md)).
