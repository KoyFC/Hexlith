# PICA200 pixels: textures, colour and depth buffers

How the GPU lays pixels out in memory, as `domain::gpu` reads and writes them in `pixel_formats.h`. The sources are GBATEK, 3DS GPU Texture Formats, 3DS GPU Internal Registers - Framebuffer registers, 3DS GPU External Registers - Memfill and Memcopy, and 3DS Video Texture Swizzling; where GBATEK leaves something open, Azahar's `src/video_core/texture/` and `src/common/color.h` are the source, and the table says so.

## Tiles

Textures, colour buffers and depth buffers are stored in 8x8 tiles, a row of tiles at a time, and the pixels of a tile in Morton order: the bits of x and y interleaved, x's lowest first, so that the tile is four 4x4 quarters, each four 2x2 blocks, each four pixels left to right and then down (GBATEK, Video Texture Swizzling, whose examples `domain/video/morton.h` is tested against). Pixel (x, y) of a buffer w pixels wide starts at

    (8 w (y / 8) + 64 (x / 8) + morton(x mod 8, y mod 8)) * bytes per pixel

y counting the rows in the order they are stored. GPUREG_RENDERBUFFER_BLOCK32 can make the tiles of a colour buffer 32x32; the reference title never sets it.

## Texture formats

By the value of GPUREG_TEXUNITi_TYPE. Bits are numbered in the little-endian word of the texel's size; red is the most significant.

| Value | Format | Bytes a texel | Layout | Source |
| --- | --- | --- | --- | --- |
| 0 | RGBA8 | 4 | red 31-24, green 23-16, blue 15-8, alpha 7-0 | GBATEK |
| 1 | RGB8 | 3 | red 23-16, green 15-8, blue 7-0; alpha FFh | GBATEK |
| 2 | RGB5A1 | 2 | red 15-11, green 10-6, blue 5-1, alpha 0 | GBATEK |
| 3 | RGB565 | 2 | red 15-11, green 10-5, blue 4-0; alpha FFh | GBATEK |
| 4 | RGBA4 | 2 | red 15-12, green 11-8, blue 7-4, alpha 3-0 | GBATEK |
| 5 | LA8 | 2 | luminance 15-8 to red, green and blue, alpha 7-0 | GBATEK |
| 6 | HILO8 | 2 | red 15-8, green 7-0; blue 0, alpha FFh | GBATEK |
| 7 | L8 | 1 | luminance; alpha FFh | GBATEK |
| 8 | A8 | 1 | alpha; red, green and blue 0 | GBATEK |
| 9 | LA4 | 1 | luminance 7-4, alpha 3-0 | GBATEK |
| 10 | L4 | ½ | luminance; alpha FFh. Two texels a byte, the first in the low nibble | GBATEK; Azahar for the nibble order |
| 11 | A4 | ½ | alpha; red, green and blue 0. Nibbles as L4 | GBATEK; Azahar for the nibble order |
| 12 | ETC1 | ½ | 64 bits for each 4x4 block | GBATEK; the Khronos ETC1 specification it cites |
| 13 | ETC1A4 | 1 | 64 bits of alpha, a nibble a pixel, then an ETC1 block | GBATEK |

A narrow channel widens by repeating its top bits: 4 bits times 11h, 5 bits shifted up three with their top three below, 6 bits shifted up two with their top two below, 1 bit to 0 or FFh.

**ETC1.** An 8x8 tile is four 4x4 blocks, left to right and then down. Within a block the pixels are numbered a column at a time, 0 to 3 down the first column, and the block is little-endian, not big-endian as the ETC1 specification stores it. Bits 0 to 15 choose each pixel's small or large modifier and bits 16 to 31 negate it; bit 32 splits the block into top and bottom halves instead of left and right; bit 33 selects differential mode; bits 34 to 36 and 37 to 39 are the modifier tables of the second and first halves. In individual mode each half has its own 4-bit red, green and blue; in differential mode the first half has 5-bit values and the second adds a signed 3-bit difference to each. The modifier pairs are (2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183), and the result is clamped to a byte (GBATEK, ETC1).

A differential sum outside five bits is something GBATEK says never happens. Azahar keeps it to a byte and widens it as a byte, and Hexlith does the same.

## Colour buffer, transfer and LCD formats

Two registers name the same five formats in different orders:

| Format | GPUREG_COLORBUFFER_FORMAT, bits 16-18 | Display transfer and LCD | Bytes |
| --- | --- | --- | --- |
| RGBA8 | 0 | 0 | 4 |
| RGB8 | 1 (Azahar; GBATEK leaves 1 unnamed) | 1 | 3 |
| RGB5A1 | 2 | 3 | 2 |
| RGB565 | 3 | 2 | 2 |
| RGBA4 | 4 | 4 to 7 | 2 |

The layouts are the texture formats of the same names. Writing a pixel keeps each channel's top bits, as Azahar's `Encode` functions do; GBATEK does not say whether the hardware rounds.

## Depth buffer formats

By GPUREG_DEPTHBUFFER_FORMAT: 0 is 16-bit depth, 2 is 24-bit depth, 3 is 24-bit depth with 8 bits of stencil (GBATEK). Depth is the low two or three bytes, little-endian, and the stencil the fourth byte (Azahar's `EncodeD24S8`).

## Compared with Azahar

`scripts/compare_pica_textures.py` decodes every texel of a set of textures on both sides: through `Pica::Texture::LookupTexture` under `third_party/azahar/oracle.patch`, and through `DecodeTexture` in `tools/hexlith-oracle-run`. On 2026-10-18:

| Textures | Texels | Agree |
| --- | --- | --- |
| 5,000 random, every format, 8 to 64 pixels a side, random bytes | 6,559,552 | all |
| The 161 the title samples before frame 1150, every mipmap level, from `hexlith-oracle-run --texture-cases-from` | 15,357,888 | all |

The title's textures are 116 RGBA8, 15 A4, 12 ETC1A4, 6 LA8, 5 RGBA4, 3 ETC1, 2 L4, 1 L8 and 1 RGB8.
