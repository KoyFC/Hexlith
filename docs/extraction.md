# Extraction

The first stage of the pipeline, and the only one that exists. `hexlith-extract` validates a cartridge image against a game profile and writes its contents into the workspace.

Nothing it produces ever enters this repository. The workspace lives outside it, for the reasons in [ADR 0007](adr/0007-no-game-data-in-the-repository.md), and the tool is given its location rather than knowing one, for the reasons in [ADR 0008](adr/0008-workspace-configuration.md).

## Running it

```sh
hexlith-extract --rom-dir ~/recomp-data/rom --work-dir ~/recomp-data/work
```

Both paths can come from the environment instead, as `HEXLITH_ROM_DIR` and `HEXLITH_WORK_DIR`, or from `$XDG_CONFIG_HOME/hexlith/workspace.conf`:

```
rom_dir = /home/someone/recomp-data/rom
work_dir = /home/someone/recomp-data/work
```

The command line wins, then the environment, then the file. There is no default for either. When nothing provides a path the tool names every source it consulted and stops, because the failure worth avoiding is a tool that invents a location and writes a cartridge's worth of content next to whatever it was run from.

`--image` names a file directly. Without it the tool looks in the ROM directory for a single `.3ds` or `.cci` file and refuses to choose when there is more than one.

`--allow-mismatch` extracts an image the profile does not match. It exists for development and the tool says so on the way out.

## What it does, in order

1. Reads the image. The file is opened read-only, and a symbolic link is refused so that a path cannot be redirected after it was checked.
2. Parses the NCSD header and takes the partition the profile names. Nothing scans for the first usable one.
3. Parses the NCCH header. An encrypted partition is refused here: the project handles no keys and implements no decryption.
4. Parses the extended header, which is the only thing that says whether `.code` is compressed.
5. **Validates against the profile and stops if anything disagrees.** Nothing has been written at this point, so a refused image leaves the work directory as it was.
6. Writes the extended header, the logo and the plain region.
7. Parses the ExeFS and writes its files, decompressing `.code` when the extended header's flag is set.
8. Parses the RomFS, writes every file and creates every directory, including the ones the image declares with nothing in them.
9. Checks what was written against the profile again: the RomFS file count, the module count, and the size and hash of the decompressed `.code`.
10. Writes `manifest.txt` and `modules.txt`.

Step 5 asks what the image claims about itself and step 9 asks what came out of it. They are different questions and both are worth asking.

## The layout it produces

Under `<work>/<program-id>-v<remaster-version>/`, which for the reference cartridge is `0004000000055D00-v00`:

| Path | Contents |
| --- | --- |
| `manifest.txt` | Every file written, with its size and SHA-256 |
| `modules.txt` | The CRO-family modules, summarised from their headers |
| `exheader.bin` | The extended header region, 0x800 bytes |
| `logo.bin` | The logo region |
| `plain.txt` | The plain region |
| `exefs/.code` | The executable, decompressed |
| `exefs/banner`, `exefs/icon` | The title's presentation files |
| `romfs/` | The read-only filesystem, as a directory tree |

The version in the directory name is the extended header's remaster version, which is the revision of this build of the executable. It is not the NCCH version field, which is a format revision shared by every NCCH, and not the card info header's title version, which describes the cartridge. All three are read and all three are checked; only one of them identifies the code being lifted.

**The modules are not written a second time.** All 134 of them already live in the RomFS, so extracting the RomFS extracts them, and a copy in a directory of its own would be a second thing to keep in step with the first. `modules.txt` is the index that says which of the extracted files are modules and what their headers declare.

## The manifest format

Line oriented, one record per line, in the shape [ADR 0006](adr/0006-analysis-layer-format.md) chose: the kind first, then `key=value` fields, with `path=` last so that everything after it is the path and a name holding a space cannot be read as another field. Lines beginning with `#` are comments.

```
manifest-version value=1
profile program-id=0004000000055D00 remaster-version=00 name=Pokemon X
image size=0x80000000 digest=6ac1cdfa...
partition index=0 offset=0x4000 size=0x6b59e000
romfs files=653 directories=37
totals files=659 bytes=0x6aa5c64f
file size=0x4eb000 digest=0f6d3e1d... source=exefs decompressed=yes path=exefs/.code
```

`modules.txt` carries three records per module: `module` for the name and segment totals, `module-tables` for every count its header declares, and one `module-segment` per entry of its segment table.

### Determinism

**Two runs over the same image produce both files byte for byte identical.** That is a requirement rather than an observation, and it is why the manifest holds nothing from the clock, nothing from the order the host filesystem returns entries in, and no absolute paths. Records are sorted by path and every number is fixed-width hexadecimal.

The test suite asserts it against a synthetic image, and the reference cartridge was run twice to confirm it.

## One number that looks wrong and is not

The SHA-256 of `exheader.bin` is not the extended header hash the NCCH header stores. The stored hash covers 0x400 bytes, which is the half the size field declares, while the region on disk and the file written are 0x800. [`formats/exheader.md`](formats/exheader.md) measures both halves and explains what the second one is.

## What it does not do

It implements no decryption and handles no keys. An encrypted partition is an error.

It verifies no hashes beyond the ones it uses for identity. The RomFS IVFC tree and the CRR module hashes are not checked, and the parsers skip them: verifying title integrity would need the keys the project refuses to handle, and nothing downstream depends on it.

It reads no relocation or import table. The CRO parser records the count each one declares, which is a plain header field, and stops there. Those entry layouts are not written down in [`formats/`](formats/) yet, and nothing is read out of a format this project has not recorded. The lifter and the `ldr:ro` reimplementation are what need them, and that is where they will be verified.

## Measured against the reference cartridge

| | |
| --- | --- |
| Files written | 659 |
| Bytes written | 1,789,249,103 |
| RomFS files | 653 in 37 directories |
| CRO-family modules | 134 |
| `.code`, compressed | 2,858,188 |
| `.code`, decompressed | 5,156,864 |
| Time | a little over two seconds |

The decompressed size equals the sum of the three page-aligned segments in the extended header exactly, and its SHA-256 is the one an independently written Python implementation produced during the initial survey. [`formats/exefs.md`](formats/exefs.md) records both.
