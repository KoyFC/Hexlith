# NCCH

One content partition. The executable partition of a game is a CXI, which is an NCCH whose content type has the executable bit set.

Source: `3dbrew NCCH`.

## Header

| Offset | Size | Field |
| --- | --- | --- |
| 0x000 | 0x100 | RSA-2048 signature of the header, using SHA-256 |
| 0x100 | 4 | Magic `NCCH` |
| 0x104 | 4 | Content size, in media units |
| 0x108 | 8 | Partition ID |
| 0x110 | 2 | Maker code |
| 0x112 | 2 | Version |
| 0x114 | 4 | Content lock seed verification value |
| 0x118 | 8 | Program ID |
| 0x120 | 0x10 | Reserved |
| 0x130 | 0x20 | Logo region SHA-256 hash |
| 0x150 | 0x10 | Product code |
| 0x160 | 0x20 | Extended header SHA-256 hash |
| 0x180 | 4 | Extended header size, in bytes |
| 0x184 | 4 | Reserved |
| 0x188 | 8 | Flags |
| 0x190 | 4 | Plain region offset, in media units |
| 0x194 | 4 | Plain region size, in media units |
| 0x198 | 4 | Logo region offset, in media units |
| 0x19C | 4 | Logo region size, in media units |
| 0x1A0 | 4 | ExeFS offset, in media units |
| 0x1A4 | 4 | ExeFS size, in media units |
| 0x1A8 | 4 | ExeFS hash region size, in media units |
| 0x1AC | 4 | Reserved |
| 0x1B0 | 4 | RomFS offset, in media units |
| 0x1B4 | 4 | RomFS size, in media units |
| 0x1B8 | 4 | RomFS hash region size, in media units |
| 0x1BC | 4 | Reserved |
| 0x1C0 | 0x20 | ExeFS superblock SHA-256 hash |
| 0x1E0 | 0x20 | RomFS superblock SHA-256 hash |

All region offsets are relative to the start of the NCCH, not to the start of the file.

The reserved word at 0x1AC sits between the ExeFS and RomFS groups. Reading the RomFS fields at 0x1B8 instead of 0x1B0 yields the RomFS hash region size and a zero, which looks like a title with no RomFS. This mistake was made and caught during the initial survey, which is why the offsets are written out here.

## Flags at 0x188

| Byte | Meaning |
| --- | --- |
| 3 | Crypto method, selecting the AES keyslot for the secondary key |
| 4 | Content platform: 1 for CTR, 2 for New 3DS |
| 5 | Bits 0 and 1 are the content form, bits 2 to 7 the content type |
| 6 | Content unit size, computed as 0x200 << value |
| 7 | Bitmask: 0x1 FixedCryptoKey, 0x2 NoMountRomFs, 0x4 NoCrypto |

Hexlith requires bit 0x4 of byte 7 and refuses anything else. The project handles no keys and implements no decryption, so an encrypted partition is an error and not a case to work around.

The executable bit is bit 1 of byte 5, which is byte 0x18D of the header.

## Observed in the reference image

Partition 0 reports product code `CTR-P-EKJA`, program ID `0004000000055D00`, maker code `01`, version 2, content type 0x03 which is data and executable, and the NoCrypto flag set. Its extended header size field reads 0x400 while the region on disk is 0x800, which [`exheader.md`](exheader.md) explains.
