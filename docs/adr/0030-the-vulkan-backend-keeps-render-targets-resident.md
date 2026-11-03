# 0030. The Vulkan backend keeps render targets resident and writes them back when something reads them

Status: Accepted. Amends [ADR 0022](0022-the-vulkan-backend-keeps-gpu-memory-true.md), whose choice of copying each target in from memory before a list and back after it is replaced; its fragment stage, textures and vertex paths stand.
Date: 2026-11-03

## Context

[ADR 0022](0022-the-vulkan-backend-keeps-gpu-memory-true.md) chose to copy each draw target's colour and depth buffers from memory into Vulkan images before drawing and back afterwards, so that nothing drawn could be stale. [`docs/gpu-speed.md`](../gpu-speed.md) measured what that costs on the whole boot to frame 1101 on the Intel GPU: 1,909 round trips of about a megabyte each way, 8.2 s converting between the PICA's tiled formats and the images' (4.1 s each way) and 7.1 s waiting for the device, of the 27 s the window adds to a run that takes 33 s without one.

Most of those trips move data nobody reads. The colour a list draws is read by the display transfer that follows it, once a frame. The depth it draws is read by nothing, and the next list clears it with a memory fill.

[ADR 0028](0028-the-live-display-draws-in-the-titles-own-memory.md) made the live display see every command gsp::Gpu runs before it runs, and the port now says, through `BeforeRead` and `BeforeWrite`, what each is going to read and write.

## Decision

**A target's images stay on the device between lists.** The backend keeps up to sixteen targets, each with the images of its own, identified by both buffers' addresses and formats and its size, and the least recently used is written back and dropped to make room. The colour and the depth of a target are tracked apart, each in one of three states:

- **Stale.** Memory is the truth and the image is unknown. A new target starts so.
- **Clean.** The image and memory hold the same.
- **Dirty.** The image is newer than memory.

**Memory is made true when something reads it.** `BeforeRead` writes back the dirty parts that overlap the range. So does a texture about to be decoded, and so does the upload of another target over the same bytes. Only the parts that overlap are written back, and the colour alone is for the display transfer.

**An overwrite makes a part stale.** `BeforeWrite` marks the overlapping parts stale; a dirty part is written back first unless the write covers it whole, in which case the bytes it holds are going to be replaced and are dropped. The common case is a memory fill of the depth buffer.

**A target another target's bytes overlap is not trusted.** Drawing to a target writes back the dirty parts of any other that overlaps it and marks them stale, since memory is about to change under them.

**A write nobody announced is noticed.** Each part keeps a copy of the bytes it last agreed with memory on, and before a target is drawn to again memory is compared with it. If memory differs the part is stale, whatever it was, and a dirty part that differs is counted as a conflict. The CPU writes game memory without telling anyone, and lifted code stores through a pointer, which is why the alternative of tracking those was rejected ([ADR 0028](0028-the-live-display-draws-in-the-titles-own-memory.md)). On a conflict memory wins, which is what the old order gave when the CPU wrote after a list had drawn. The count is reported and the boot to frame 1101 is expected to give none.

**The run ends with memory true.** `LiveDisplay::Finish` reads the whole of GPU memory, and the backend writes back what is dirty when it is destroyed.

**Each group still waits for the device,** as before. A draw has to be finished before anything reads it, a display transfer reads the colour once a frame, and a queue of submissions in flight would need its host buffers and descriptor set kept apart per submission, for a gain that is the overlap of the GPU's work with the game's own, not the cost measured.

## Why not the alternatives

**Azahar's surface cache,** with textures as cached surfaces, formats reinterpreted and the cache invalidated by the CPU's writes. It would remove the texture decoding as well, and it is a large piece of code whose mistakes show as stale pictures, the reason ADR 0022 gave for not starting there. The targets alone are what the measurement says costs most.

**Tracking the guest's writes** to find what changed, in every store of lifted code and the interpreter, which ADR 0028 rejected for the same cost.

**Keeping ADR 0022 and making the trip cheaper,** converting on the GPU or in several threads. The trips themselves are what is wrong: most of what they move is never read.

**Hashing the bytes instead of keeping a copy** to notice unannounced writes. A hash of a megabyte costs about what comparing it with a copy does, and the copy says nothing more than the hash does here; the copy is kept because a later change can use it to find which bytes the CPU wrote.

## Consequences

A read the game makes of GPU memory that nothing announced sees what the last writeback left, not what the GPU has drawn since. The title does not do this, as far as it has been run, and the golden trace, the instruction count and the tick count of a boot to frame 1101 with the window are held to the interpreter's to say so. A game that did would behave as it does with the reference renderer in the window, which draws in the same memory but synchronously.

The vertex buffers a draw reads are read from memory as they were, and a vertex buffer inside a render target that is dirty would be read stale. A draw samples a texture inside a dirty target correctly, since decoding it writes the target back first.

Memory fills and the other commands that touch targets are cheaper to the backend than before, and the first list after a fill of the colour buffer uploads it again. Clearing the image in place when a fill covers the target is the next step if the measurement says it matters.
