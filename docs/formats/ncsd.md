# NCSD

The outermost container of a cartridge image. A `.3ds` or `.cci` file is an NCSD holding up to eight NCCH partitions.

Source: `3dbrew NCSD`.

## Header

| Offset | Size | Field |
| --- | --- | --- |
| 0x000 | 0x100 | RSA-2048 SHA-256 signature of the header |
| 0x100 | 4 | Magic `NCSD` |
| 0x104 | 4 | Image size, in media units |
| 0x108 | 8 | Media ID |
| 0x110 | 8 | Partition filesystem types |
| 0x118 | 8 | Partition crypt types |
| 0x120 | 0x40 | Partition table: eight pairs of offset and length, in media units |
| 0x160 | 0x20 | ExHeader SHA-256 hash |
| 0x180 | 4 | Additional header size |
| 0x184 | 4 | Sector zero offset |
| 0x188 | 8 | Partition flags |
| 0x190 | 0x40 | Partition ID table |
| 0x1D0 | 0x20 | Reserved |
| 0x1FE | 1 | Anti-piracy support flag, from system version 9.6.0-X |
| 0x1FF | 1 | Save crypto determination flag |

A media unit is 0x200 bytes. Every offset and size in the table is expressed in media units, so each needs multiplying by 0x200 before use.

## Partition assignments

| Index | Content |
| --- | --- |
| 0 | Executable content, a CXI |
| 1 | Electronic manual, a CFA |
| 2 | Download Play child container, a CFA |
| 6 | New 3DS update data, a CFA |
| 7 | Update data, a CFA |

A partition with a zero offset and a zero length is absent. Hexlith selects the partition the GameProfile names rather than taking the first usable one, so that an unexpected cartridge layout is reported instead of silently accepted.

## Card Info Header

Present in cartridge images, immediately after the NCSD header.

| Offset | Size | Field |
| --- | --- | --- |
| 0x200 | 4 | Writable address for CARD2, or 0xFFFFFFFF for CARD1 |
| 0x204 | 4 | Card info bitmask |
| 0x300 | 4 | Filled size of the cartridge |
| 0x310 | 2 | Title version |
| 0x312 | 2 | Card revision |
| 0x320 | 8 | Title ID of the CVer in the included update partition |
| 0x328 | 2 | Version number of that CVer |
| 0x1000 | 0x200 | InitialData |

## Observed in the reference image

Partitions 0, 1 and 7 are present and partitions 2 through 6 are empty, which matches a retail cartridge with no Download Play child. The image size field accounts for the whole 2 GiB file.
