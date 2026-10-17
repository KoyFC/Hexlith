# 0016. The GPU recording keeps the lists and the pages that changed

Status: Accepted
Date: 2026-10-17

## Context

Phase 7 is to draw the title screen with Vulkan from what the game hands the GPU, without running the game. What the game hands the GPU is GX commands: memory fills, display transfers, texture copies and command lists. A command list is a run of register writes, and among them are addresses: of vertex buffers, index buffers, textures, shader code, the framebuffers. The bytes at those addresses are in VRAM and in the linear heap, and the game writes them between lists.

So a recording of the lists alone cannot be drawn. It says which buffers to read and not what they held.

## Options

**The lists alone.** Small and simple. Phase 7 would have to run the game to have the memory, which is what the recording was meant to spare it.

**The lists and the memory each reads.** Exact and as small as it can be, but it means decoding the PICA registers that hold addresses and the sizes behind them, which is Phase 7's own work done early and done without a way to check it.

**The lists, and before each list every page of GPU-visible memory that changed since the one before.** No decoding: a page is compared with what it held when it was last recorded. More than a list needs, and everything any list could need.

## Decision

The third. Every GX command is recorded as the game wrote it, with its frame. Before each command list, every mapped page of VRAM and of the linear heap is compared with a copy of what it held when last recorded, a page never recorded counting as zeros, and those that differ are recorded; then the list's bytes. Pages are stored once per content, named by their SHA-256, so a page that returns to an old content costs a line in the index and nothing more.

Replaying the index in order rebuilds GPU-visible memory as it stood before every list. `docs/formats/gpu-recording.md` is the layout.

## Consequences

Phase 7 can draw any frame of the recording from the recording alone, by applying the pages up to a list and then running it.

Recording is slow, because every page is compared before every list. The boot to the title screen takes 19.5 seconds recorded against 1.7 without, so it is done only when asked, with `hexlith-boot --record-gpu`.

Memory a GX command changes, a fill or a copy, is in the next list's pages as well as in the command. A replay that runs the commands and applies the pages gets the same memory either way; the pages are the ones to trust, since display transfers convert nothing here.

What the game writes to a buffer after submitting the list that reads it, and before the GPU would have read it, is not seen. Whether the reference title ever does that has not been checked.
