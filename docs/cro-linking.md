# Linking modules offline

The console's `ldr:ro` service maps a CRO module where the game asks, then patches every relocated word in it and in the modules already loaded. `src/domain/linker` does the same computation without a console: given the modules and the addresses chosen for them, it works out the value each relocated word ends up holding and which module supplied it.

It writes nothing. The answer is a list of words per module, and putting them into an image or into guest memory is a separate step. The analysis wants the list, a comparison with a memory dump wants an image, and the `ldr:ro` reimplementation will want guest memory, so the computation is kept apart from all three.

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

**Unloading** is not implemented. An unload resets every import that pointed into the module, so linking just the resident modules in the order they were loaded reaches the state the console reached by loading and unloading. That is enough to reproduce a moment of a run. The service will need the real thing.

**Named lookups** go through the export tree on the console, and a module with no tree finds nothing whatever its export table holds. Hexlith keeps that rule but searches the table itself, which gives the same answer whenever the tree indexes the table correctly.

## The data segment

A word of the data segment ends up at its address in the data buffer, and that is the address the linker reports. The console gets there by a detour. It patches internal relocations into data while the data is still in the file, and the game copies the data to the buffer afterwards. An external relocation into data is patched straight into the buffer, and the copy then overwrites it. That is the bug `LoadCRO_New` fixed. The reference title calls the old command, and no external relocation of it patches data, so the detour changes nothing here. [`formats/cro0.md`](formats/cro0.md) has the measurement.

## On the reference title

All 133 loadable modules link together onto the static module without an error, at arbitrary non-overlapping addresses. All 21,868 external relocations resolve, including the static module's 331: every symbol any module imports by position is supplied by a module the title ships. Nothing a module imports is left pointing at the unresolved hook once everything is loaded.

That says the import graph is closed. It does not say that the words hold the right values. The check for that is the next section.

## Checking against memory

`scripts/compare_cro_link.py` compares the linker with the memory of a game Azahar is running. It needs the Debug level of `Service.LDR`, because only there does `LoadCRO` log the data and BSS buffers it was given, and Azahar's GDB stub, which serves the memory.

1. In `qt-config.ini`, `log_filter=*:Info Service.LDR:Debug` and `use_gdbstub=true`, port 24689.
2. Play to the moment to be compared and leave the game running.
3. Write the dump commands from the log as it stands, then connect, dump and detach. Connecting pauses the game.

```sh
D=~/recomp-data/work/<id>/traces/dump-<moment>
scripts/compare_cro_link.py gdb ~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/log/azahar_log.txt $D > $D.gdb
gdb -batch -ex 'set architecture arm' -ex 'target remote 127.0.0.1:24689' -x $D.gdb
scripts/compare_cro_link.py compare $D --romfs ~/recomp-data/work/<id>/romfs --tool build/ci/tools/hexlith-cro-link/hexlith-cro-link
```

The first command also writes the resident modules and their addresses beside the dumps, so the comparison does not depend on the log surviving the next run.

What is compared, and what counts as a failure:

- Each resident module's code and read-only data, byte by byte. A relocated word has to hold the value the linker computes, and every other byte has to be the file's. A byte that differs elsewhere means the loader did something the linker does not model.
- Each relocated word in the module's data buffer. The other bytes of data are counted and not judged, because the game writes to its own data.
- Each word of the main executable that the static module's own relocations patch, which tests the unresolved hook as well as the resolved imports.

Connecting and then resuming the game with `continue` closed the emulation in one attempt on 2026-10-04, so the procedure connects only once the game is where it should be, and detaches when the dump is written.
