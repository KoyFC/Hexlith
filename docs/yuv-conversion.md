# YUV to RGB conversion

The title plays video from the frame after its title screen modules load, and y2r:u turns every frame of it from YUV into RGB. This records where the conversion comes from, how it was checked, and what the title asks of it.

## Sources

GBATEK, 3DS Video Y2R Registers, describes the unit: five input formats, four output formats, a clockwise rotation in quarter turns, output either as lines or as 8x8 tiles in Morton order, five unsigned multipliers and three signed offsets, and conversion in strips of eight lines. Its formula multiplies the samples and adds the offsets afterwards instead of subtracting 128 from U and V first. GBATEK, 3DS Video Texture Swizzling, gives the order of a tile, which `domain/video/morton.h` is tested against.

The rounding is Azahar's (`src/core/hw/y2r.cpp`): the products are shifted right by three, the offset and 0x18 are added, and the sum is shifted right by five and clamped to a byte. Azahar's comment says this matched hardware bit for bit as far as it could be tested. Azahar is also the source for what a rotation does to a strip whose blocks are not square, and for the order of the blocks after a half or three-quarter turn, which GBATEK does not describe.

3dbrew, Y2R Services, could not be read while this was written: the site is unreachable from the development machine's network.

## The comparison

`scripts/compare_y2r.py` generates random cases and has both sides convert them: Azahar through `HW::Y2R::PerformConversion`, under the oracle hooks of `third_party/azahar/oracle.patch`, and Hexlith through `video::ConvertYuvToRgb`, with `tools/hexlith-oracle-run`. A case is a random input format, output format, rotation and layout, a width of 8 to 64 pixels, 1 to 24 lines (a multiple of eight for tiles, even for 4:2:0), random samples, a random alpha, and coefficients that are a standard set in three cases out of ten, random within the registers' ranges in four, and any signed halfwords in the rest.

On 2026-10-18, 20,000 cases with seed 7 agreed byte for byte, as did 200 with seed 1.

What the comparison does not reach: the gathering of samples and the scattering of pixels in units with gaps between them, which y2r:u does and Azahar's oracle bypasses with a unit of one sample and no gap. Those have unit tests of their own. Dithering is not modelled on either side.

Hexlith refuses an odd number of lines in 4:2:0, where the last line needs chroma the unit is never sent. Azahar reads whatever its buffer held; what the hardware does is unknown.

## What the title asks for

Every conversion from a new game to frame 620 is the same: 4:2:0 with byte samples, RGBA8 out, no rotation, 8x8 tiles, 320 by 240, an alpha of 255, and the third standard set, ITU Rec. 601 with scaling. The output goes in units of one strip, 10,240 bytes, with a gap of 6,144 after each, which makes it the left 320 columns of a texture 512 wide.

Until frame 930 every frame converted has a luma of 16 and a chroma of 128 everywhere, the black of a video signal. The first frame with a picture in it is converted at frame 932, the frame the title screen first shows, and from then on one is converted every frame the title draws: the video is the title screen's background. Azahar does the same: a constant luma until the vertical blank 1106, the frame its title screen first shows, and a picture from then on. The constant is not the same: Azahar's luma averages 10 where Hexlith's is 16 everywhere, in a buffer neither draws. Why has not been looked into.
