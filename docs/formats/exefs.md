# ExeFS

A small container holding the executable and the title's presentation files. It lives at the offset the NCCH header gives at 0x1A0.

Source: `3dbrew ExeFS`.

## Header

The header occupies the first 0x200 bytes of the ExeFS region.

| Offset | Size | Content |
| --- | --- | --- |
| 0x000 | 0xA0 | File headers, up to ten entries of 0x10 bytes |
| 0x0A0 | 0x20 | Reserved |
| 0x0C0 | 0x140 | File hashes, up to ten SHA-256 values |

Each file header:

| Offset | Size | Field |
| --- | --- | --- |
| 0x0 | 8 | File name, NUL padded |
| 0x8 | 4 | Offset, relative to the end of the header |
| 0xC | 4 | Size, in bytes |

Unused headers are zeroed. A file's data therefore begins at the ExeFS offset plus 0x200 plus the entry's offset.

The hash table is stored in reverse order against the file headers, so the hash at 0x1E0 belongs to the first file and the one at 0x0C0 to the tenth.

Azahar's extractor, and the TriAevum script derived from it, iterate only eight entries rather than ten. That is harmless in practice because `.code` is always the first entry, but it is wrong against the format and Hexlith iterates all ten.

## The `.code` compression

When bit 0 of the extended header flags byte at 0x0D is set, the `.code` section is compressed. 3dbrew names the algorithm reverse-LZSS and gives no further detail, so the authoritative description is Azahar's implementation.

The encoding is read from the end of the buffer towards the beginning, and output is written from the end of the output buffer towards the beginning. A footer in the last eight bytes describes the layout:

| From the end | Size | Field |
| --- | --- | --- |
| -8 | 4 | Packed field: the high byte is the footer size, the low three bytes the encoded size |
| -4 | 4 | Additional size, which added to the compressed length gives the decompressed length |

Decoding walks backwards. Each control byte carries eight flags, consumed from the most significant bit. A clear flag copies one literal byte. A set flag reads a two-byte back-reference, where the top nibble plus three is the run length and the low twelve bits plus two is the distance forward in the already-written output.

The decompressed buffer starts as a copy of the compressed input placed at its front, because the encoding is designed to be decompressed in place.

### Verification

The decoder was run against the reference image. The compressed section is 2,858,188 bytes and the output is 5,156,864 bytes, which equals the sum of the three page-aligned segment sizes in the extended header exactly. An output that agrees with an independently stated size to the byte is strong evidence the decoder is correct.

## Observed in the reference image

Three entries are present: `.code`, `banner` and `icon`. The remaining seven headers are zeroed.
