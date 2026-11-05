# 0033. The display transfer runs on the device, and its output is written to memory when read

Status: Accepted. Amends [ADR 0028](0028-the-live-display-draws-in-the-titles-own-memory.md), whose live display did display transfers on the title's memory; a transfer the backend cannot carry out is still done there.
Date: 2026-11-05

## Context

[ADR 0032](0032-groups-of-draws-are-submitted-without-waiting.md) lets a list go on while the device draws. A display transfer undid most of that: the live display announced its input to the backend, which wrote the whole colour buffer back, waiting for it and for every group before it, converted it a pixel at a time into the PICA's tiles in memory, and then ran the transfer on that memory. In a boot to frame 1101 that was 1,320 waits, 1.1 s of conversion and 0.4 s of transfers.

The title makes one transfer. Of the 1,321 up to frame 1101, 1,320 are RGBA8 in tiles to RGB8 in rows, with no flip and no scale, flags `0x1004`, an input of 256 by 512 and an output of 240 by 400 or 240 by 320 ([`docs/gpu-speed.md`](../gpu-speed.md)). The other, in frame 0, is RGBA4. The input is not the start of the target's colour buffer: it begins on a row of tiles, 112 rows down for the 400 row output and 192 for the 320, so that the screen's rows are the last of the 512 the target has.

## Decision

**The live display offers each transfer to the backend first** (`GpuBackend::RunDisplayTransfer`), before announcing anything. A backend that takes it owes memory the output when anything reads it; one that declines leaves it to the live display, which announces and does it on memory as before. No other backend takes one.

**The Vulkan backend takes the title's transfer and no other.** RGBA8 in tiles to RGB8 in rows, no flip, no scale, no texture copy, an output of whole tiles no wider than its target, read from a row of tiles of the colour of a resident target that is not stale. It copies that rectangle of the target's image into a buffer, in a submission it does not wait for, and keeps the transfer as pending with the range it writes and what memory held over it. A transfer it does not take, such as the RGBA4 one, goes to memory.

**The output is written when something reads it.** `BeforeRead`, a texture about to be decoded, a draw's upload over the same bytes and the end of the run all find the pending transfers that overlap and make them true: wait for the copy, which the queue runs after the groups before it, and convert its pixels to RGB8 into memory. Where memory is still what it held when the transfer was made, the output goes in. Where someone has written since, that write is the later of the two and stays, and the backend counts it as a conflict, as it counts a write nobody announced to a dirty target ([ADR 0030](0030-the-vulkan-backend-keeps-render-targets-resident.md)). An overwrite that covers a pending output whole drops it without writing, and a partial one makes it true first.

**The conversion stays on the CPU.** The first plan made the conversion a compute shader. What is left to do on the host is a loop over the 240 by 400 pixels at resolve time, and with it the boot to frame 1101 spent 0.13 s converting back to memory, where it spent 1.1 s before, the two transfers memory still does included. A pipeline, a layout and an image usage were not worth the rest. The device does what cost most, the wait and the conversion of the whole buffer into tiles. If a boot says otherwise, the loop moves to a shader without changing what is promised here.

**Composing the screens stays on the CPU,** reading memory ([ADR 0027](0027-the-screens-are-drawn-as-the-game-runs-and-shown-in-a-window.md)). Presenting from the device would tie the window to one backend.

## Why not the alternatives

**Writing the output into memory at once,** by waiting for the copy. It is what the wait was.

**Keeping the transfer's output only on the device and having the screens read it from there.** It saves the conversion too, and makes the window depend on the backend and on a second path for the pictures to be held to the reference's.

**Accepting more shapes,** a flip, a scale or other formats. The title makes none, and each would be code that nothing runs.

## Consequences

A read of the transfer's output that nothing announced sees what memory held before it. The title reads it through the live display, which announces the screens' ranges before taking a frame's picture, and through the end of the run. A game that read its framebuffer behind the live display's back would see it stale, as it would see a colour buffer the backend has not written back.

The backend's statistics count the transfers it carried out and the waits, and `hexlith-boot` reports both.
