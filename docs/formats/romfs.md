# RomFS

The title's read-only data filesystem, wrapped in an IVFC hash tree. It lives at the offset the NCCH header gives at 0x1B0.

Sources: `3dbrew RomFS`, and `azahar/src/core/file_sys/ncch_container.cpp` for the level 3 offset.

## IVFC header

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Magic `IVFC` |
| 0x04 | 4 | Magic number, 0x10000 |
| 0x08 | 4 | Master hash size |
| 0x0C | 8 | Level 1 logical offset |
| 0x14 | 8 | Level 1 hash data size |
| 0x1C | 4 | Level 1 block size, as a log2 |
| 0x20 | 4 | Reserved |
| 0x24 | 8 | Level 2 logical offset |
| 0x2C | 8 | Level 2 hash data size |
| 0x34 | 4 | Level 2 block size, as a log2 |
| 0x38 | 4 | Reserved |
| 0x3C | 8 | Level 3 logical offset |
| 0x44 | 8 | Level 3 hash data size |
| 0x4C | 4 | Level 3 block size, as a log2 |
| 0x50 | 4 | Reserved |
| 0x54 | 4 | Reserved |
| 0x58 | 4 | Optional info size |

## Finding the filesystem

**The level 3 filesystem begins 0x1000 bytes into the RomFS region.** Azahar hardcodes this:

```cpp
u32 romfs_offset = (ncch_header.romfs_offset * block_size) + 0x1000;
u32 romfs_size = (ncch_header.romfs_size * block_size) - 0x1000;
```

The logical offset fields in the IVFC header are **not** file offsets. They address the hash level space. On the reference image level 1 reports a logical offset of zero, which is where the IVFC header itself sits, and level 3 reports 0xD66000, which contains neither a valid level 3 header nor anything else meaningful. Using a logical offset as a file offset produces a parse that fails in a confusing way rather than an obvious one.

Confirmed by observation. At RomFS + 0x1000 the header length field reads 0x28 as required, the entropy of the surrounding bytes is 3.02 which is consistent with a table of offsets rather than with hash data, the directory walk yields 653 plausible filenames, and a file read back through the resulting offsets produced a valid CRO module header whose embedded name matched its filename. At RomFS + 0x1000 + 0xD66000 the header length field reads 0x1E7F5, which is not a header.

The hash levels for this title are stored after level 3, block aligned, which was confirmed by finding entropy above 7.1 in that region. Hexlith does not read them. The project does not verify title integrity, and the hashes are the only part of the RomFS that would need the keys the project refuses to handle. Their exact ordering was therefore not determined.

## Level 3 header

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Header length, 0x28 |
| 0x04 | 4 | Directory hash table offset |
| 0x08 | 4 | Directory hash table length |
| 0x0C | 4 | Directory metadata table offset |
| 0x10 | 4 | Directory metadata table length |
| 0x14 | 4 | File hash table offset |
| 0x18 | 4 | File hash table length |
| 0x1C | 4 | File metadata table offset |
| 0x20 | 4 | File metadata table length |
| 0x24 | 4 | File data offset |

All offsets are relative to the start of level 3.

## Directory metadata entry

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Parent directory offset, pointing at itself for the root |
| 0x04 | 4 | Next sibling directory offset |
| 0x08 | 4 | First child directory offset |
| 0x0C | 4 | First file offset |
| 0x10 | 4 | Next directory in the hash bucket |
| 0x14 | 4 | Name length, in bytes |
| 0x18 | varies | Name, UTF-16LE, padded to a multiple of four bytes |

## File metadata entry

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | Containing directory offset |
| 0x04 | 4 | Next sibling file offset |
| 0x08 | 8 | File data offset, relative to the file data region |
| 0x10 | 8 | File data length |
| 0x18 | 4 | Next file in the hash bucket |
| 0x1C | 4 | Name length, in bytes |
| 0x20 | varies | Name, UTF-16LE, padded to a multiple of four bytes |

An offset of 0xFFFFFFFF terminates a sibling chain. The root directory is the entry at offset zero of the directory metadata table and has no name.

Names are UTF-16LE, so an extractor has to transcode them and cannot assume one byte per character. Hexlith also has to reject names that would escape the output directory, since a filesystem image is untrusted input.

The hash tables exist so the console can look a path up without walking the tree. An extractor that walks the tree does not need them, which is what Hexlith does, since it reads every entry exactly once.

## Observed in the reference image

653 files in 1,797,873,664 bytes. The root holds 133 CRO modules, `static.crs` and a `.crr` directory, described in [`cro0.md`](cro0.md). Bulk data sits under `a/`, as numbered GARC archives, with video under `m/`, audio under `sound/` and shader binaries under `shaders/`.
