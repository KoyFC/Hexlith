# NCCH extended header

The metadata that describes how to load the executable and what it is allowed to do. It immediately follows the NCCH header.

Sources: `3dbrew NCCH/Extended_Header`, plus observation for the on-disk size.

## The size discrepancy

The NCCH header field at 0x180 gives the extended header size as 0x400, which covers the System Control Info and the Access Control Info. That is the region the header's SHA-256 hash is taken over.

The region actually present on disk is 0x800 bytes. The second half is the access descriptor, which carries a signature over the permissions the title requests, so that the kernel can check the title is not asking for more than it was granted.

Verified by observation on the reference image, measuring the Shannon entropy of each quarter:

| Offset | Size | Content | Entropy, bits per byte |
| --- | --- | --- | --- |
| 0x000 | 0x200 | System Control Info | 2.43 |
| 0x200 | 0x200 | Access Control Info | 3.73 |
| 0x400 | 0x100 | Access descriptor signature | 7.19 |
| 0x500 | 0x100 | NCCH header RSA public key | 7.17 |
| 0x600 | 0x200 | Access Control Info, limitation copy | 3.71 |

The two high-entropy blocks are cryptographic material. The limitation copy at 0x600 holds the same service list as the real ACI but a wildcard program ID, `00040000FFFFFF00` on the reference image, which is what makes it identifiable as the descriptor's copy rather than a duplicate of the real one.

Azahar's extractor reads 0x800 for this reason. Hexlith does the same and records both numbers so the difference is not mistaken for a bug.

## System Control Info, at 0x000

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | Application title, defaulting to `CtrApp` |
| 0x08 | 5 | Reserved |
| 0x0D | 1 | Flags: bit 0 CompressExefsCode, bit 1 SDApplication |
| 0x0E | 2 | Remaster version |
| 0x10 | 0xC | Text code set info: address, physical region size in pages, size |
| 0x1C | 4 | Stack size |
| 0x20 | 0xC | Read-only code set info |
| 0x2C | 4 | Reserved |
| 0x30 | 0xC | Data code set info |
| 0x3C | 4 | BSS size |
| 0x40 | 0x180 | Dependency module list, 48 title IDs |
| 0x1C0 | 0x40 | System info: save data size, jump ID, reserved |

Bit 0 of the flags byte is what tells the loader that the ExeFS `.code` section is compressed. Nothing in the ExeFS itself indicates it.

The three code set entries give the virtual address each segment loads at. These are the addresses the lifted code will use, so they belong in the GameProfile.

## Access Control Info, at 0x200

| Offset | Size | Field |
| --- | --- | --- |
| 0x000 | 0x170 | ARM11 local system capabilities |
| 0x170 | 0x80 | ARM11 kernel capabilities |
| 0x1F0 | 0x10 | ARM9 access control |

ARM11 local system capabilities, relative to 0x200:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | Program ID |
| 0x08 | 4 | Core version |
| 0x0C | 1 | Flag1 |
| 0x0D | 1 | Flag2 |
| 0x0E | 1 | Flag0 |
| 0x0F | 1 | Priority |
| 0x10 | 0x20 | Resource limit descriptors |
| 0x30 | 0x20 | Storage info |
| 0x50 | 0x100 | Service access control, 34 entries of 8 bytes |
| 0x150 | 0x10 | Extended service access control |
| 0x160 | 0xF | Reserved |
| 0x16F | 1 | Resource limit category |

The service access control list is the authoritative statement of which Horizon services a title uses, which makes it the work list for the HLE layer. A service absent from this list cannot be reached by the title at all.

Flag0, at 0x20E, packs three fields: the core the main thread prefers in bits 0 and 1, the cores it may run on in bits 2 and 3, and the Old 3DS memory mode in bits 4 to 7. Mode 0 gives the application a 64 MiB region of FCRAM. Source: 3dbrew, NCCH/Extended Header.

ARM11 kernel capabilities, at 0x370, are 28 descriptors of one word each, followed by 0x10 reserved bytes. A descriptor's type is the number of ones it starts with before its first zero, and an unused one is all ones. Source: 3dbrew, NCCH/Extended Header, ARM11 Kernel Capabilities.

| Leading ones | Type | Fields |
| --- | --- | --- |
| 3 | Interrupt info | |
| 4 | System call mask | Bits 24 to 26 pick a table of 24 calls, bits 0 to 23 allow them: table n covers calls 24n to 24n + 23 |
| 6 | Kernel release version | Major in bits 8 to 15, minor in bits 0 to 7 |
| 7 | Handle table size | Bits 0 to 18 |
| 8 | Kernel flags | Memory type in bits 8 to 11, 1 for the application region |
| 9 | Map an address range | Two descriptors, start and exclusive end |
| 11 | Map one I/O page | Page index in bits 0 to 19 |

The kernel version matters beyond bookkeeping: the kernel puts the linear heap at `0x14000000` for a title declaring a version below 2.44 and at `0x30000000` otherwise. [`horizon-memory.md`](../horizon-memory.md) has the source.

## Observed in the reference image

The application title is `kujira-1`. CompressExefsCode is set and SDApplication is not. Thirty dependency module title IDs are listed, the save data size is 1 MiB, and the jump ID equals the program ID.

The service access control list holds 28 entries. `ldr:ro` is among them, which is what established before any module was parsed that the title loads CRO modules at runtime. The extended service access control list is empty.

Flag0 is `0x04`: ideal processor 0, affinity mask 1, memory mode 0. The main thread priority is `0x30`.

The kernel capabilities declare kernel version 2.35, a handle table of 512 entries, memory type 1, three system call mask tables and six address range descriptors, with the remaining descriptors unused. The masks allow 51 calls: `0x01` to `0x03`, `0x06`, `0x08` to `0x0C`, `0x0F`, `0x11`, `0x13` to `0x25`, `0x27` to `0x32`, `0x35` to `0x3D`. Every one of the 32 calls the walked code of the main executable makes is among them, which is a check on both the parser and that count.

Segment addresses and sizes are recorded in the GameProfile rather than here, since they are version-specific data rather than format knowledge.
