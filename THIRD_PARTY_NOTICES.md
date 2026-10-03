# Third-party notices

Origins and attributions for code and documentation Hexlith takes from elsewhere.

## Current state

**No third-party code has been incorporated yet.** The repository currently contains only original contributions.

This file exists from the start so that the first adoption has somewhere to go, and so that the format is settled before anyone is in a hurry.

## Reference material

Hexlith's format documentation is written against these sources. Reading documentation to learn a file layout creates no licensing obligation, but the sources are recorded because the project rule is that no format or address is asserted without one.

| Source | Used for |
| --- | --- |
| [3dbrew](https://www.3dbrew.org/) | The NCSD, NCCH, extended header, ExeFS, RomFS and CRO0 formats |
| [Azahar](https://github.com/azahar-emu/azahar), `GPL-2.0-or-later` | The CRO header field layout from `src/core/hle/service/ldr_ro/cro_helper.h`, and the RomFS level 3 offset from `src/core/file_sys/ncch_container.cpp` |
| [TriAevum](https://github.com/coccofresco/TriAevum), `GPL-3.0-or-later` | Architecture reference. Audited in `docs/triaevum-audit.md` |

## Format for an entry

When code is adopted, it gets an entry here before the commit that adopts it, and the files keep their original copyright headers and SPDX identifiers.

```
### <Upstream project>

Upstream:  <URL>
Licence:   <SPDX identifier>
Copyright: <holders as stated upstream>
Files:     <paths in this repository>
Taken at:  <upstream commit or release>
Changes:   <what was modified, and why>
```

Donor code is vendored under a `third_party/` directory with its own licence file rather than being mixed into Hexlith's sources, so that the boundary stays visible in the file tree and not only in a notice.

## Not covered

Nothing derived from the game is adopted from any project under any licence. See `LICENSE_SCOPE.md`.
