# Container and module formats

Reference notes for the formats Hexlith reads. One file per format.

These exist because the project rule is that no format, offset or address is ever invented. Every field here carries a source, and where the documentation and the actual data disagree, the disagreement is written down rather than quietly resolved.

## Sources

- **3dbrew**, at <https://www.3dbrew.org/>, is the primary reference for the CTR container formats. Cited as `3dbrew NCCH` and so on.
- **Azahar**, at <https://github.com/azahar-emu/azahar>, GPL-2.0-or-later, is the reference implementation. Cited with a file path when a constant comes from the code rather than from documentation.
- **Observation** means measured from the reference cartridge image in this project. Cited as `observed`. Observations record addresses, sizes and hashes, never content.

Where a note says `observed`, the measurement is reproducible from the reference ROM with the extraction tool, and the value belongs in the GameProfile rather than here.

## Files

| File | Format |
| --- | --- |
| [`ncsd.md`](ncsd.md) | NCSD, the cartridge image and its partition table |
| [`ncch.md`](ncch.md) | NCCH, one content partition |
| [`exheader.md`](exheader.md) | The NCCH extended header: code layout and permissions |
| [`exefs.md`](exefs.md) | ExeFS and the backwards-LZSS compression of `.code` |
| [`romfs.md`](romfs.md) | RomFS and its IVFC wrapper |
| [`cro0.md`](cro0.md) | CRO dynamic modules, CRS and CRR |

Hexlith's own analysis tables are not a container format and are documented in [`../analysis-tables.md`](../analysis-tables.md).
