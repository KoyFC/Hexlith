# 0032. Groups of draws are submitted without waiting for the device

Status: Accepted. Amends [ADR 0030](0030-the-vulkan-backend-keeps-render-targets-resident.md), whose last decision, that each group still waits for the device, is replaced; the rest of it stands.
Date: 2026-11-05

## Context

[ADR 0030](0030-the-vulkan-backend-keeps-render-targets-resident.md) kept each group of draws waiting for the device, on the reasoning that a display transfer needs the colour at once and that a queue of submissions in flight would need its buffers and descriptor set kept apart for each. [`docs/gpu-speed.md`](../gpu-speed.md) measured what the wait is made of, with the device's own timestamps from Mesa's `INTEL_MEASURE`: in a boot to frame 1101 with the window on the Intel GPU the host waited 3.99 s, the device worked at least 1.49 s of it, and the rest, at most 2.50 s, is what each of 3,229 submissions costs whatever it holds, about 0.77 ms. That cost does not fall with faster drawing. It falls only with fewer waits.

A group's work is also not needed at once. The colour it draws is read by the display transfer that follows the list, but the list after it has no use for it, and the title submits a list, goes on running its own code for a good part of a frame, and only then asks for the transfer.

## Decision

**A group of draws is submitted and the list goes on.** `VulkanContext` hands back a `Ticket` for a submission it has queued with a fence, `Wait` blocks until a ticket's submission has finished, and `Finished` asks without blocking. The queue runs submissions in order, so waiting for one is waiting for all before it.

**Four groups may be with the device at once.** The backend keeps a ring of four frames. A frame is what a group owns until the device is done with it: the staging, vertex, input, draw-word and texel buffers, and the descriptor set that points at two of them. A group takes the next frame, and waits only if the group that last used it has not finished, which is the one case in which a list waits. Four is a first choice, not a measured one: a frame of the title submits a few groups, and the number is a constant to be changed when a boot's count of waits for the ring says so.

**What the host waits for otherwise is what it reads.** Writing a target back copies its image into a buffer the backend keeps for that, waits for that copy, which the queue runs after every group before it, and converts what it holds. `BeforeRead`, a texture about to be decoded and the upload of another target over the same bytes all go through it, as in ADR 0030. A target's state stays what ADR 0030 says: dirty from the moment a group is submitted, since from then the image is newer than memory.

**Submissions are ordered by memory barriers, not by waiting.** Each group's first barrier on an image takes the writes of the group before it as its source, the copy of a writeback begins with a barrier from the attachments' writes to the transfer read, and the images' layouts are the ones the render pass already leaves them in. A slot is destroyed, when the least recently used is dropped to make room, only after the last submission has finished, and so is the backend, whose destructor writes back what is dirty first.

## Why not the alternatives

**A thread of its own that submits and waits.** The game's thread would not wait, and the thread would, with a queue between them, and the buffers a list is read from would be shared across threads. It costs a thread and the synchronisation that goes with it, for what a fence and a ring do on the game's own thread.

**A ticket for each part, with a writeback waiting for the one that made it dirty.** The copy is queued after the draw and cannot finish before it, so waiting for the copy's own ticket is waiting for the draw as well, and the part has nothing to remember.

**A ring larger than four.** Each frame holds buffers that grow to the largest group met, and the title does not draw more than a few groups ahead of a transfer. It can be raised when a boot says it waits for the ring.

## Consequences

A list's draws are on the device when `RunCommandList` returns and in memory only when something reads them. That is already how ADR 0030 treats the images, and the title does not read what it drew without a display transfer, whose range the live display announces to the backend before it runs.

While the display transfer is done on memory, which it still is until [ADR 0033](0033-the-display-transfer-runs-on-the-device-and-its-output-is-written-when-read.md), it announces its input to the backend and so makes the backend write the colour back, waiting for it and for every group before it. The waits this ADR removes are the ones in between, and what it leaves is the same wait once for each transfer.

`waits` in the backend's statistics counts the times the host stopped for the device, so that a change to the ring or to what waits is visible in a boot's report.
