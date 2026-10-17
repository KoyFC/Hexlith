# GPU recordings

What `hexlith-boot --record-gpu` writes to `<work-dir>/<title>/gpu/`: the GPU's work as the game handed it over, in the order it did. It is Hexlith's own format. [ADR 0016](../adr/0016-the-gpu-recording-keeps-lists-and-changed-pages.md) says why it holds what it holds. A recording is derived from the game and stays in the workspace.

## Layout

| Path | What it is |
| --- | --- |
| `recording.txt` | The index, one event a line, in order |
| `lists/NNNNNN.bin` | Each command list's bytes, numbered from zero in the order submitted |
| `pages/<sha256>.bin` | A 4 KiB page of memory, named by the SHA-256 of its bytes in lower-case hexadecimal |

## The index

The first line is `# hexlith gpu recording, format 1`. Every other line starts with the frame it happened in, the count of the top screen's vertical blanks since the process started, as in the golden trace. Numbers other than the frame are hexadecimal, eight digits.

| Line | Meaning |
| --- | --- |
| `<frame> gx <w0> ... <w7>` | A GX command as the game wrote it in the command queue, eight words. The low byte of the first is the command: 0 DMA, 1 command list, 2 memory fill, 3 display transfer, 4 texture copy, 5 cache flush. The words after it are its parameters (`gsp_command.h` in Azahar; 3dbrew, GSP Shared Memory) |
| `<frame> page <address> <sha256>` | The page at that virtual address now holds the content in `pages/<sha256>.bin` |
| `<frame> list <address> <size> <file>` | The command list the preceding `gx` line submitted, its address and size, and its bytes in `lists/<file>` |

A command list's `gx` line comes first, then a `page` line for every page of VRAM and of the linear heap that changed since the last list, then its `list` line. A page that has never been recorded counts as zeros, so the first list's pages are every page that is not.

## Replaying

Start with GPU-visible memory all zeros and read the index in order, writing each page as it comes. When a `list` line is reached, memory is as it stood when the game submitted that list. The other GX commands are there for their order and their parameters: what a fill or a copy did to memory also appears in the pages before the next list.

## Measured

Recorded on 2026-10-17 from a new game, with two taps of A through the language selection, to the moment the title screen's last module has loaded, at frame 227:

| | |
| --- | --- |
| Command lists | 1,155, from 32 to 29,824 bytes, 8,183,856 bytes in all |
| Memory fills | 446 |
| Display transfers | 445 |
| Cache flushes | 1,155, one before each list |
| DMA and texture copies | none |
| Pages recorded | 6,886 |
| On disk | 22 MB |
