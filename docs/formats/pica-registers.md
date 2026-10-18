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
