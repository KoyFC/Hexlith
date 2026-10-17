# Linking modules offline

The console's `ldr:ro` service maps a CRO module where the game asks, then patches every relocated word in it and in the modules already loaded. `src/domain/linker` does the same computation without a console: given the modules and the addresses chosen for them, it works out the value each relocated word ends up holding and which module supplied it.

It writes nothing. The answer is a list of words per module, and putting them into an image or into guest memory is a separate step. The analysis wants the list, a comparison with a memory dump wants an image, and the `ldr:ro` service wants guest memory, so the computation is kept apart from all three.

The source for every step is `azahar/src/core/hle/service/ldr_ro/cro_helper.cpp`. The table layouts it reads are in [`formats/cro0.md`](formats/cro0.md).

## What a load does

`LoadCRO` is given three addresses: where to map the file, and where the module's data and BSS go (3dbrew, `RO:LoadExeCRO`, request words 2, 4 and 7). Then, in this order:

1. **Rebase the segments** (`RebaseSegmentTable`). Code and read-only data move with the module, so their address is the mapping address plus their file offset. Data and BSS go to the buffers the game gave.
2. **Reset the external relocations** (`ResetExternalRelocations`). Every word an import will patch is first written as though the import had resolved to the module's OnUnresolved hook. A module without one, which is most of them, leaves the addend alone.
3. **Apply the internal relocations** (`ApplyInternalRelocations`). Each word receives the address of a segment of the same module plus the addend.
4. **Resolve `__aeabi_atexit`** against the export `nnroAeabiAtexit_` (`ApplyExitRelocations`).
5. **Import** (`ApplyImportNamedSymbol`, `ApplyModuleImport`). Named imports are searched for in every linked module, the static one first and then the others in load order, and the first export of that name wins. Indexed and anonymous imports are taken from the module their import entry names, if it is loaded.
6. **Export** (`ApplyExportNamedSymbol`, `ApplyModuleExport`). Every module already linked, the static one included, gets whatever it imports from the new one.
7. **Register** the module at the end of the list.

The static module is linked first and once, the way `LoadCRS` does it: its segments are already at their virtual addresses, and its own imports from the modules are reset to its OnUnresolved hook, which in the reference title is a real function in the main executable.

## Where Hexlith stops

**Relocation types.** `ApplyRelocation` implements an absolute address in two forms and a relative address, and stops on the branch types. Hexlith computes the same three and refuses the rest, because no source this project has read says what they do. The reference title uses only the first.

**Static anonymous symbols**, a module handing positions to the static module, are refused. Azahar notes that no game is known to use them, and the reference title declares none, so nothing could check an implementation.

**Linking by hand**, `LinkCRO` and `UnlinkCRO`, and the second load command, `LoadCRO_New`, are not implemented. The reference title reaches none of them on the way to the title screen.

**Named lookups** go through the export tree on the console, and a module with no tree finds nothing whatever its export table holds. Hexlith keeps that rule but searches the table itself, which gives the same answer whenever the tree indexes the table correctly.

## The service

`services::RelocatableObjects` is `ldr:ro` for the game. The words it writes are the linker's. The rest of what the console's loader does to a module is in `linker::RebaseModuleImage` and `linker::FixModuleImage`, which work on a copy of the module's bytes, and in the service, which works on the address space:

- **Map.** The game's buffer is mapped at the address it asked for, read-only and marked as code, and the buffer itself is closed to the game while the module lives there.
- **Rebase the header and the tables.** Every offset in the header becomes an address. So do the offsets inside the tables that point into other tables: the names of exports and imported modules, the slices of the import tables that belong to each imported module, and the batch each import resolves. The segment table gets the segment addresses. The first entry of each batch of external relocations is marked unresolved (`RebaseHeader`, `RebaseSegmentTable`, `RebaseExportNamedSymbolTable`, `RebaseImportModuleTable`, the three `RebaseImport...SymbolTable`s, `ResetExternalRelocations`).
- **Relocate and link**, writing the linker's words and marking every batch an import resolved, in the new module and in every module it exports to. After a later load only the words that load changed are written again, so a word the game has since copied elsewhere is not touched twice.
- **Register.** The static module's header holds the head of two lists, the auto-linked modules and the rest, and each module's header holds its neighbours. The head's previous field points at the tail (`Register`).
- **Fix.** At a fix level above zero the magic becomes `FIXD` and the tables past the level's barrier are emptied. The part of the buffer past the end of what is kept goes back to the game. The reference title asks for level 1 and its first module keeps all four of its pages (`Fix`).
- **Make the code executable** (`GetExecutablePages`).

An unload undoes it in the order `UnloadCRO` runs its steps:

- **Unregister.** The module leaves its list, whichever of the four cases of head, middle, tail or only module it is (`Unregister`).
- **Unlink**, in `linker::Linker::Unload`. Every import batch of the module is reset to its own OnUnresolved address and marked unresolved. Then, in the static module and every module linked automatically, a named import is reset if the module being unloaded exports that name, and every import by index or by position from a module of its name is reset to the importer's OnUnresolved address (`ResetImportNamedSymbol`, `ResetImportIndexedSymbol`, `ResetImportAnonymousSymbol`, `ResetExportNamedSymbol`, `ResetModuleExport`). The named case looks only at the name, so an import another module supplied is reset too; Azahar does this and so does Hexlith. A fix level of 2 or more emptied the import tables, and level 3 the exports, and an unload then resets nothing from them.
- **Clear the relocations** of a module that was never fixed, every word back to zero (`ClearRelocations`). The reference title fixes every module it loads, at level 1, so this does not run.
- **Unrebase**, in `linker::UnrebaseModuleImage`: every address in the header and tables is an offset again, the list fields and the fixed size go to zero, and the magic stays `FIXD`. A segment's address loses the module's address whatever it points into, so the data segment's entry ends up holding the data buffer's address minus the module's, and a BSS segment's becomes zero (`Unrebase`).
- **Unmap** what is still mapped, which gives the buffer back to the game as it is (`Unmap`).

The reference title unloads `DllLangSelect` at frame 225 of a new game and loads the title screen's three modules into the same place two frames later, reusing the buffer.

## The data segment

A word of the data segment ends up at its address in the data buffer, and that is the address the linker reports. The console gets there by a detour. It patches internal relocations into data while the data is still in the file, and the game copies the data to the buffer afterwards. An external relocation into data is patched straight into the buffer, and the copy then overwrites it. That is the bug `LoadCRO_New` fixed. The reference title calls the old command, and no external relocation of it patches data, so the detour changes nothing here. [`formats/cro0.md`](formats/cro0.md) has the measurement.

## On the reference title

All 133 loadable modules link together onto the static module without an error, at arbitrary non-overlapping addresses. All 21,868 external relocations resolve, including the static module's 331: every symbol any module imports by position is supplied by a module the title ships. Nothing a module imports is left pointing at the unresolved hook once everything is loaded.

That says the import graph is closed. It does not say that the words hold the right values. The check for that is the next section.

## Checking against memory

`scripts/compare_cro_link.py` compares the linker with the memory of a game Azahar is running. It needs the Debug level of `Service.LDR`, because only there does `LoadCRO` log the data and BSS buffers it was given, and Azahar's GDB stub, which serves the memory.

1. In `qt-config.ini`, `log_filter=*:Info Service.LDR:Debug` and `use_gdbstub=true`, port 24689.
2. Play to the moment to be compared and leave the game running.
3. Write the dump commands from the log as it stands, then connect, run some of them, and detach. Connecting pauses the game.

```sh
D=~/recomp-data/work/<id>/traces/dump-<moment>
scripts/compare_cro_link.py gdb ~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/log/azahar_log.txt $D > $D.gdb
head -1 $D.gdb > $D.first.gdb
gdb -batch -ex 'set architecture arm' \
    -ex 'set remote memory-read-packet-size 512' -ex 'set remote memory-read-packet-size fixed' \
    -ex 'target remote 127.0.0.1:24689' -x $D.first.gdb -ex detach
scripts/compare_cro_link.py compare $D --romfs ~/recomp-data/work/<id>/romfs --tool build/ci/tools/hexlith-cro-link/hexlith-cro-link
```

The first command also writes the resident modules and their addresses beside the dumps, so the comparison does not depend on the log surviving the next run.

Azahar's stub is the limit. A read of more than a few hundred bytes in one packet overflows its reply buffer, which is what the packet size setting is for. At that size it delivers about 5 KB a second, and on 2026-10-04 it froze the emulator twice, once during a read of a megabyte and once right after a read of 64 KiB. So the dumps are written in chunks of 64 KiB, one gdb command each, and the comparison uses whichever chunks exist and reports how many relocated words they left out. Getting all of a large module means several sessions, and a run of the game for each.

What is compared, and what counts as a failure:

- Each resident module's code and read-only data, byte by byte. A relocated word has to hold the value the linker computes, and every other byte has to be the file's. A byte that differs elsewhere means the loader did something the linker does not model.
- Each relocated word in the module's data buffer. The other bytes of data are counted and not judged, because the game writes to its own data.
- Each word of the main executable that the static module's own relocations patch, which tests the unresolved hook as well as the resolved imports.

Connecting and then resuming the game with `continue` closed the emulation in one attempt on 2026-10-04, so the procedure connects only once the game is where it should be, and detaches when the dump is written.

### What the first chunk showed

Captured on 2026-10-04 inside a building on the first route, with `DllField` resident at `0x006A5000` and `DllUSMirror`, which imports from it, at `0x007E5000`. One chunk was dumped before the emulator froze: the first 64 KiB of `DllField`'s code, from `0x006A5180`.

| | |
| --- | --- |
| Relocated words in the chunk | 2,880 |
| Holding the value Hexlith computes | 2,880 |
| Other bytes of code compared | 54,016 |
| Differing from the file | 0 |

The 2,880 words cover every path through the loader that this title uses. 136 are internal relocations. 2,607 are imports from the static module. 135 are imports from modules that were not loaded, holding the addend alone because `DllField` has no OnUnresolved hook. And 2 are imports from `DllUSMirror`, which was loaded after `DllField`, so they were written by the export step of the later load rather than by the import step of the earlier one.

The first words of the chunk are a run of `ldr pc, [pc, #-4]` each followed by an address in the main executable: the module's calls into the static image go through these, and the loader has patched every one.

That is 2,880 of the 14,561 words the two modules and the static module have between them, about a fifth. The rest of `DllField`, its data buffer, `DllUSMirror` and the main executable's 331 patched words are not dumped yet.

### What the service left in memory

Run from its entry point on 2026-10-17, the reference title called Initialize with the CRS and LoadCRO with `DllLangSelect` at `0x006A5000`, its data at `0x080480F0` and its BSS at `0x0804810C`, the same addresses Azahar logged on a fresh boot. The game went on to run code in the module. Once it had stopped, at the first request nothing answers yet, Hexlith's memory was compared with Azahar's dump of the same moment, `traces/dump-2026-10-16-lang-select` in the workspace, by `hexlith-boot --compare` with that directory:

| Range | Bytes | Differing |
| --- | --- | --- |
| The whole module, `0x006A5000` to `0x006A9000`, header and tables included | 16,384 | 0 |
| The main executable from `0x00190000`, which the static module patches | 65,536 | 0 |
| The module's data buffer | 28 | 0 |

That covers every word the linker writes into the module and the 315 words of the main executable the dump holds, as `compare_cro_link.py` found against Azahar, and beyond them every header field, table entry, batch flag and list pointer the service writes.

### After the unload and at the title screen

On 2026-10-17 a fresh Azahar profile was played through the language selection with two presses of A, and its memory dumped by gdb at two breakpoints: on the instruction after the game's UnloadCRO request returns, and on the instruction after its fourth LoadCRO returns, the last of the title screen's three modules. Hexlith ran the same boot from a script of two taps of A, at frames 100 and 200, and stopped at the same two places with `--stop-after cro-unload:DllLangSelect` and `--stop-after cro-load:DllSequence`. Azahar's log gave the same buffers, data and BSS addresses for all four modules as Hexlith used.

| Moment | Range | Bytes | Differing |
| --- | --- | --- | --- |
| After the unload | The buffer `DllLangSelect` was mapped from, `0x08044000`, as the game gets it back | 16,384 | 0 |
| | Its data buffer | 28 | 0 |
| | The main executable from `0x00190000` | 65,536 | 0 |
| Title screen | `DllTitle`, `0x006A5000` to `0x006C6000` | 135,168 | 0 |
| | `DllBackGround`, `0x006C8000` to `0x006D0000` | 32,768 | 0 |
| | `DllSequence`, `0x006D1000` to `0x006EA000` | 102,400 | 0 |
| | Their three data buffers | 480 | 0 |
| | The main executable from `0x00190000` | 65,536 | 0 |

The first row is the whole of what the unload leaves: the header and tables back to offsets, the list fields and the fixed size cleared, the module's own imports and the static module's imports from it reset, and the batch flags with them. The title screen's rows are each module whole, header and tables included, linked against each other and against the static module after the unload.

