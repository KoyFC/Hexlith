# Memory fills, display transfers and the screens, compared with Azahar

Between its command lists a game asks gsp::Gpu for three things that are not the PICA's work: memory fills, which clear colour and depth buffers; display transfers, which turn the tiled colour buffer the PICA drew into the linear framebuffer a screen shows, converting its format, cropping it and if asked halving it; and texture copies, which move raw bytes in lines. `domain::gpu::RunMemoryFill` and `RunTransfer` carry them out on GPU memory, `domain::gpu::MemoryFills` and `Transfer` read them from the GX commands a recording keeps, and `domain::gpu::ScreenSetup` turns the recording's framebuffer lines into the two pictures the screens show.

## Sources

GBATEK, 3DS GPU External Registers - Memfill and Memcopy, for the registers: the fill units' ranges, value and fill width, and the transfer engine's addresses, sizes and flags. GBATEK, Top/Bottom Screen and Framebuffer Setup, for a screen's two framebuffers, the one shown, and the framebuffer format, and for the LCDs being drawn from left to right.

Azahar for what GBATEK leaves open: `GPU::Execute` in `src/video_core/gpu.cpp` for how gsp::Gpu turns each GX command into register writes, and `GPU::SetBufferSwap` for a framebuffer description; `renderer_software/sw_blitter.cpp` for the arithmetic.

| What | Rule | Source |
| --- | --- | --- |
| A fill command | a unit for each of its two buffers whose start is not zero, the first's control in the low halfword of the last word | Azahar |
| Fill width | control bits 8 and 9: 16, 24, 32 bits, and 3 as 24 | GBATEK |
| The end of a fill | not included; a 16- or 24-bit fill writes its last value whole even past the end, a 32-bit one stops short | Azahar |
| The fill and transfer addresses | in 16-byte units | GBATEK; Azahar keeps 8-byte units |
| A transfer's sizes | the GX command's input size goes to `DISPLAY_GAP`, its output size to `DISPLAY_SIZE`; each holds the dimension GBATEK calls the height, 240 for a screen, in its low halfword | GBATEK and Azahar, which name the halves differently |
| Cropping | each input row is read at the input's width and only the output's width is taken | Azahar |
| A flipped, cropped transfer | its output lands lower by the cropped pixels of all its rows but one, as on hardware | Azahar, which says games rely on it |
| Conversion | tiled input becomes linear output, linear input tiled, unless flag bit 5 turns that off, when both stay | GBATEK; Azahar for bit 5 keeping both tiled or both linear |
| Downscale | 1 halves the low dimension, 2 both; a pixel averages itself and the one or three after it in memory, rounding down | GBATEK for the field; Azahar for the averaging |
| Pixel formats | the transfer order: 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 to 7 RGBA4 | GBATEK; [`formats/pica-pixels.md`](formats/pica-pixels.md) for each |
| A texture copy | flag bit 3; the size rounded down to 16 bytes; each side reads or writes its width then skips its gap, and a gap of zero makes the whole size one line | GBATEK; Azahar for the rounding and a zero gap |
| A screen | two framebuffers; a description sets the addresses of the one it names, the stride and format of the screen, and which of the two is shown | Azahar's `SetBufferSwap` |
| A screen's picture | 400 by 240 for the top, 320 by 240 for the bottom; row r of the framebuffer is column r, its first pixel at the bottom; only format bits 0 to 2 change what is read; the left eye's framebuffer | GBATEK, and the title's framebuffers coming out upright |

## Method

`scripts/compare_pica_rasterizer.py`, which [`docs/pica-rasterizer.md`](pica-rasterizer.md) describes, runs the fills and transfers too: each GX memory fill, display transfer and texture copy of the recording is a step of its own, between the lists, and Azahar carries it out through `GPU::Execute` with its software blitter. After every step both sides hash VRAM, and for a transfer whose output is outside VRAM a megabyte of memory from the output's address as well.

Random steps cover what the title never asks for: fills of one or both units, every width, ends that leave a partial value; display transfers between every pair of formats, tiled to linear and linear to tiled, with and without swizzling, flipped, cropped, downscaled each way, into VRAM or the linear heap; texture copies of random sizes, widths and gaps. The input, the output and the area around a fill are random bytes. Their addresses are multiples of 16 bytes and their ends round down the same way in 8-byte and 16-byte units, since there GBATEK and Azahar part.

## Results

On 2026-10-18:

| Steps | Agree |
| --- | --- |
| The recording to frame 1150: 4,915 lists, 1,369 memory fills and 1,369 display transfers | all |
| 2,000 random fills, display transfers and texture copies, after 500 random lists, seed 1 | all |

The comparison is not vacuous. The first run of the recording found that the fill clearing the upper half of VRAM, which ends at the very end of it, was refused: the end of a range has to be read as belonging to the memory it closes, as Azahar reads it. Ignoring the flip, as a check, made 76 of 300 random steps disagree, and averaging one pixel instead of four made 39.

## Where Hexlith departs from Azahar

| What | Azahar | Hexlith | Why |
| --- | --- | --- | --- |
| The fill and transfer addresses | 8-byte units | 16-byte units | GBATEK: bit 0 of the register is unused |
| A transfer format of 5 to 7 | stops | RGBA4 | GBATEK |
| Pixels past the end of GPU memory | read and written | skipped | |
| `GPU_MEMCOPY_FLAGS` bit 16, 32x32 tiles | ignored | ignored | the title never sets it |

GBATEK says the transfer engine hangs converting 24-bit input to anything but 24-bit output, or 16-bit input to anything but 16-bit; Azahar and Hexlith both convert. The title converts RGBA8 to RGB8 only.
