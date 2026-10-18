# PICA200 command lists and registers

A command list is how a title tells the GPU what to draw: a run of writes to the PICA200's internal registers, which the GPU reads from memory once the game submits the list through gsp::Gpu. This records the format of a list and the registers whose writes do more than store a value, as `domain::gpu::PicaState` and `CommandProcessor` read them. [ADR 0017](../adr/0017-pica-state-is-the-register-file.md) has why the state is the register file.

The primary source is GBATEK, 3DS GPU Internal Registers, cited by chapter. 3dbrew, GPU/Internal Registers, covers the same ground and could not be reached while this was written. Azahar's `src/video_core/pica/` is an oracle of behaviour: where it and GBATEK differ the difference is listed below, with which one Hexlith follows and why.

## A command

| Word | Contents |
| --- | --- |
| 0 | The first parameter |
| 1 | The header |
| 2 onwards | Further parameters, if the header asks for any |
| last | A zero word of padding if the command is an odd number of words |

The header (GBATEK, Command Lists):

| Bits | Field |
| --- | --- |
| 0 to 15 | The register ID. IDs run from 0000h to 03FFh |
| 16 to 19 | Byte mask: bit n set writes byte n of the parameter, clear keeps the register's byte |
| 20 to 27 | Parameters after the first. 0 to 255, so a command has 1 to 256 |
| 28 to 30 | Unused |
| 31 | 0: every parameter goes to the same register. 1: the parameters go to the register and the ones after it in turn |

The repeated form is how a title fills a FIFO such as the shader code port; the consecutive form is how it sets a run of registers.

## Registers that do more than store

Every write stores its value under its mask first. These then do something besides.

| ID | Name | What a write does | Source |
| --- | --- | --- | --- |
| 0010h | `GPUREG_IRQ_REQ(0)`, FINALIZE | Raises the P3D interrupt if any byte of the value equals the same byte of `IRQ_CMP(0)`, and with `IRQ_AUTOSTOP` ends the list at once. The CPU sets those two; a list cannot. Nintendo's code sets 12345678h and autostop, and Hexlith assumes that, since a recording does not hold the CPU's writes | GBATEK, Finalize Interrupt registers |
| 022Eh | `ATTR_BUF_DRAWARRAYS` | Draws, with indices counting up from `ATTR_BUF_FIRST_INDEX` | GBATEK, Geometry Pipeline registers |
| 022Fh | `ATTR_BUF_DRAWELEMENTS` | Draws, with indices from `ATTR_BUF_INDEX_LIST` | the same |
| 0232h | `FIXEDATTRIB_INDEX` | Selects the fixed attribute the data ports fill, or 0Fh for a vertex given in immediate mode, and starts the three-word queue over | the same |
| 0233h to 0235h | `FIXEDATTRIB_DATA` | Mirrors of one port. Three words make four float24 values, 96 bits with x lowest, sent highest word first. A fixed attribute then steps the index on; in immediate mode the inputs fill in turn and a vertex is drawn once `VSH_NUM_ATTR` plus one have arrived | the same |
| 023Ch, 023Dh | `CMDBUF_JUMP0`, `JUMP1` | Ends this list and goes on with the one at `CMDBUF_ADDR0` or `1` for `CMDBUF_SIZE0` or `1` bytes, both in eight-byte units | GBATEK, Command Lists |
| 025Eh | `PRIMITIVE_CONFIG` | Sets the topology in bits 8 and 9, and the primitive assembler starts over | GBATEK, Geometry Pipeline registers |
| 025Fh | `RESTART_PRIMITIVE` | The primitive assembler starts over | the same |
| 0280h, 02B0h | `GSH_BOOLUNIFORM`, `VSH_BOOLUNIFORM` | The unit's sixteen bool uniforms become bits 0 to 15 of the register | GBATEK, Shader registers |
| 0281h to 0284h, 02B1h to 02B4h | `xSH_INTUNIFORM_I0` to `I3` | The unit's integer uniform becomes the register | the same |
| 0291h to 0298h, 02C1h to 02C8h | `xSH_FLOATUNIFORM_DATA` | Mirrors of one port. Three words in float24 mode, or four in float32 mode with w first, make a uniform, stored at `FLOATUNIFORM_INDEX`'s index, which then steps on. Bit 31 of the index register selects float32 | the same; the float24 packing is the fixed attributes' |
| 029Ch to 02A3h, 02CCh to 02D3h | `xSH_CODETRANSFER_DATA` | Mirrors of one port. Each word goes to the unit's code memory at `CODETRANSFER_INDEX`, which steps on | the same |
| 02A6h to 02ADh, 02D6h to 02DDh | `xSH_OPDESCS_DATA` | The same for operand descriptors, at `OPDESCS_INDEX` | the same |
| 00B0h to 00B7h | `PROCTEX_LUT_DATA` | Mirrors of one port. Each word goes to the table and entry `PROCTEX_LUT` names, which steps on. Tables: 0 noise, 2 colour map, 3 alpha map (128 entries each), 4 colour, 5 colour difference (256 each) | GBATEK, Procedural Texture |
| 00E8h to 00EFh | `FOG_LUT_DATA` | The same for the fog table's 128 entries, at `FOG_LUT_INDEX` | GBATEK, Environment |
| 01C8h to 01CFh | `LIGHTING_LUT_DATA` | The same for the lighting tables, at the entry in bits 0 to 7 and the table in bits 8 to 12 of `LIGHTING_LUT_INDEX` | GBATEK, Fragment Lighting registers; Azahar for the table field's width |

While `VSH_COM_MODE` (0244h) bit 0 is clear and `GEOSTAGE_CONFIG` (0229h) says no geometry shader is in use, every write to the vertex shader's uniforms, code, descriptors and output mask reaches the geometry shader's as well. GBATEK names the register and gives no description; Azahar's command processor is the source.

## Registers a draw reads

What the reference renderer of [`docs/pica-rasterizer.md`](../pica-rasterizer.md) takes from the registers when it draws. Names and IDs are GBATEK's, by the chapter of 3DS GPU Internal Registers given; a field whose position GBATEK does not give, or gives differently, says where it came from.

| ID | Name | Fields | Source |
| --- | --- | --- | --- |
| 0040h | `FACECULLING_CONFIG` | bits 0 and 1: 0 and 3 keep both windings, 1 clockwise, 2 counter-clockwise | Rasterizer registers |
| 0041h, 0043h | `VIEWPORT_WIDTH`, `VIEWPORT_HEIGHT` | half the size, float24 in bits 0 to 23 | the same |
| 0047h, 0048h to 004Bh | `FRAGOP_CLIPPING`, `CLIP_COEF` | bit 0 enables the plane; four float24 coefficients | the same |
| 004Dh, 004Eh | `DEPTHMAP_SCALE`, `DEPTHMAP_OFFSET` | float24 | the same |
| 004Fh, 0050h to 0056h | `SH_OUTMAP_TOTAL`, `SH_OUTMAP_O0` to `O6` | the count in bits 0 to 2; a semantic in bits 0 to 4 of each byte, 1Fh none | Shader registers, Shader Output |
| 0065h to 0067h | `SCISSORTEST_MODE`, `_POS`, `_DIM` | mode 0 off, 1 outside the box, 3 inside it; x in bits 0 to 9, y in 16 to 25, the second corner inclusive | Rasterizer registers |
| 0068h | `VIEWPORT_XY` | signed x in bits 0 to 9, y in 16 to 25 | the same |
| 006Dh | `DEPTHMAP_ENABLE` | bit 0 clear: a W-buffer | the same |
| 0080h | `TEXUNIT_CONFIG` | bits 0 to 2 enable units 0 to 2; bit 10 the procedural texture; bit 13 gives unit 2 unit 1's coordinates | Texturing registers |
| 0081h to 0085h, 008Eh; 0091h to 0096h; 0099h to 009Eh | `TEXUNITi_BORDER_COLOR`, `_DIM`, `_PARAM`, `_LOD`, `_ADDR1`, `_TYPE` | border RGBA; height in bits 0 to 10, width in 16 to 26; magnification filter bit 1, minification bit 2, wrap t bits 8 to 10, wrap s 12 to 14, mipmap filter bit 24, unit 0's type 28 to 30; LOD bias fixed1.4.8 in bits 0 to 12, greatest level 16 to 19, least 24 to 27; address in eight-byte units; format in bits 0 to 3 | the same; Azahar for wrap modes 4 to 7 |
| 00C0h to 00C4h, 00C8h, 00D0h, 00D8h, 00F0h, 00F8h | `TEXENVi_SOURCE`, `_OPERAND`, `_COMBINER`, `_COLOR`, `_SCALE` | sources in nibbles 0 to 2 and 4 to 6; colour modifiers in nibbles 0 to 2, alpha modifiers in bits 12 to 14, 16 to 18, 20 to 22; colour and alpha operations in bits 0 to 3 and 16 to 19; the constant RGBA; scales in bits 0 and 1, 16 and 17 | Environment |
| 00E0h | `TEXENV_UPDATE_BUFFER` | fog mode in bits 0 to 2, 5 fog; which of stages 0 to 3 write the buffer's colour in bits 8 to 11 and alpha in 12 to 15; the fog's depth flipped by bit 16 | the same |
| 00E1h, 00FDh | `FOG_COLOR`, `TEXENV_BUFFER_COLOR` | RGB, RGBA | the same |
| 0100h | `COLOR_OPERATION` | fragment operation mode in bits 0 and 1, 0 the usual one; bit 8 blends rather than applying the logic operation | Framebuffer registers |
| 0101h | `BLEND_FUNC` | colour and alpha equations in bits 0 to 2 and 8 to 10; source and destination factors for colour in 16 to 19 and 20 to 23, for alpha 24 to 27 and 28 to 31 | the same |
| 0102h, 0103h | `LOGIC_OP`, `BLEND_COLOR` | operation in bits 0 to 3; the constant RGBA | the same |
| 0104h | `FRAGOP_ALPHA_TEST` | enable bit 0, function 4 to 6, reference 8 to 15 | the same |
| 0105h, 0106h | `STENCIL_TEST`, `STENCIL_OP` | enable bit 0, function 4 to 6, write mask 8 to 15, reference 16 to 23, input mask 24 to 31; the actions on failing the stencil test, failing the depth test and passing, in bits 0 to 2, 4 to 6 and 8 to 10 | the same |
| 0107h | `DEPTH_COLOR_MASK` | depth test enable bit 0, function 4 to 6, red to alpha written by bits 8 to 11, depth written by bit 12 | the same |
| 0113h, 0115h | `COLORBUFFER_WRITE`, `DEPTHBUFFER_WRITE` | colour written when bits 0 to 3 are not all clear; depth and stencil when bits 0 and 1 are not | the same; Azahar for which bits count |
| 0116h, 0117h | `DEPTHBUFFER_FORMAT`, `COLORBUFFER_FORMAT` | depth format in bits 0 and 1; colour format in bits 16 to 18 | the same, and [`pica-pixels.md`](pica-pixels.md) |
| 011Ch to 011Eh | `DEPTHBUFFER_LOC`, `COLORBUFFER_LOC`, `FRAMEBUFFER_DIM` | addresses in eight-byte units; width in bits 0 to 10, height less one in 12 to 21 | the same |
| 0140h to 01BFh | `LIGHTi_*` | per light, 10h words apart: specular 0 and 1, diffuse and ambient, each blue, green and red in ten bits with 255 one; x and y float16 at +4, z at +5; the spot direction as signed fixed1.1.11 at +6 and +7; directional bit 0, two-sided bit 1 and geometric factors bits 2 and 3 at +9; distance attenuation bias and scale, float20, at +Ah and +Bh | Fragment Lighting registers |
| 01C0h, 01C2h | `LIGHTING_AMBIENT`, `LIGHTING_NUM_LIGHTS` | global ambient as a light colour; the count less one in bits 0 to 2 | the same |
| 01C3h | `LIGHTING_CONFIG0` | shadow enable bit 0, Fresnel to the primary and secondary alphas bits 2 and 3, the configuration 4 to 7, shadow on primary and secondary 16 and 17, shadow inverted 18, shadow alpha 19, bump texture 22 and 23, shadow texture 24 and 25, clamp highlights 27, bump mode 28 and 29, bump renormalisation disabled 30 | the same |
| 01C4h | `LIGHTING_CONFIG1` | per light, shadow disabled in bits 0 to 7, spot attenuation 8 to 15, distance attenuation 24 to 31; tables D0, D1, FR, RB, RG and RR disabled by bits 16, 17, 19, 20, 21 and 22 | the same; Azahar swaps RB and RR (ADR 0019) |
| 01C6h | `LIGHTING_DISABLE` | bit 0: no lighting | the same; ADR 0019 for why `LIGHTING_ENABLE` is not read |
| 01D0h to 01D2h | `LIGHTING_LUTINPUT_ABS`, `_SELECT`, `_SCALE` | a nibble per table, D0, D1, SP, FR, RB, RG, RR in turn: the absolute value off in its bit 1; the input in bits 0 to 2; the scale in bits 0 to 2, 0 to 3 for one to eight, 6 a quarter, 7 a half | the same |
| 01D9h | `LIGHTING_LIGHT_PERMUTATION` | the light in each of eight slots, three bits a nibble | the same |
| 0200h to 0226h | `ATTR_BUF_*` | the base in eight-byte units; each attribute's type and size and the fixed ones in the format words; each array's offset, components, stride and count | Geometry Pipeline registers |
| 0227h, 0228h, 022Ah | `ATTR_BUF_INDEX_LIST`, `ATTR_BUF_NUMVERTICES`, `ATTR_BUF_FIRST_INDEX` | the index buffer's offset in bits 0 to 27, halfword indices when bit 31 is set; the count; the first index of a draw that has no index buffer | the same |
| 0229h | `GEOSTAGE_CONFIG` | 2 in bits 0 and 1 when a geometry shader runs | the same |
| 024Ah, 0252h | `VSH_OUTMAP_TOTAL1`, `GSH_MISC0` | the vertex shader's outputs less one in bits 0 to 3; the geometry shader's mode in bits 0 to 7, 0 taking a fixed number of outputs at a time | the same; Azahar for the mode's name and meaning |
| 025Eh | `PRIMITIVE_CONFIG` | topology in bits 8 and 9: list, strip, fan, geometry shader | the same |
| 0289h to 028Dh, 02B9h to 02BDh | `xSH_INPUTBUFFER_CONFIG`, `_ENTRYPOINT`, `_ATTRIBUTES_PERMUTATION`, `_OUTMAP_MASK` | inputs less one in bits 0 to 3; the entry in bits 0 to 15; a register for each input, a nibble each; the outputs written | Shader registers |

## Where Azahar reads a list differently

None of these differences shows in a list the reference title writes. They are recorded because the comparison with Azahar over random lists would otherwise report them as mistakes.

| Point | GBATEK | Azahar | Hexlith |
| --- | --- | --- | --- |
| Register IDs 0300h to 03FFh | shader units 2 and 3 live there | writes are dropped | stored, with no effect |
| The value a FIFO port takes under a partial byte mask | not described | the whole value, before the mask | Azahar's, since nothing says otherwise |
| `FLOATUNIFORM_INDEX` | index in bits 0 to 7 | bits 0 to 6 | Azahar's; the two agree for the 96 uniforms there are |
| float24 uniform packing | written as "ZZWWWWWW, YYYYZZZZ, XXXXXXYY", which GBATEK marks as unsure of | the fixed attributes' packing | the fixed attributes' packing, which GBATEK describes precisely and Azahar agrees with |
| `CODETRANSFER_INDEX`, `OPDESCS_INDEX` | bits 0 to 11 | the whole word | Azahar's |
| `FIXEDATTRIB_INDEX` past 0Fh | bits 0 to 3 | the whole word; an attribute is dropped | Azahar's |
| A jump written by the first parameter of a command that has more | not described | the rest of the command's parameters are read from the start of the new list | the rest are the old list's, then the new list starts |
| A command whose parameters run past the end of the list | not described | reads past the end | writes the parameters there are and stops |
| A lighting LUT table number from 24 to 31 | not described | out of bounds | the word is dropped |
| `IRQ_REQ(1)` to `(15)`, 0011h to 001Fh | each compares with its own `IRQ_CMP` | stored only | stored only |
