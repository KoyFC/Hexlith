# Measuring where modules land

[ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) chose to lift CRO modules position-independently, and named what would settle whether the faster alternative is also safe: a run of the title recording every `ldr:ro` load and unload with the module name and the resulting base address.

This note is how to produce that recording and what the recording showed. The answer is that it is not safe: the same module lands at different addresses within one run, and the ADR is no longer provisional. The last section has the run.

## No instrumentation is needed

The ADR assumed an instrumented build. It is not necessary. Azahar logs the fact already, in `LoadCRO`:

```
[   3.072226] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO "DllLangSelect" loaded at 0x006A5000, fixed_end=0x006A9000
```

and the matching line when a module goes away:

```
[ 188.610105] Service.LDR <Info> ldr_ro.cpp:UnloadCRO:369: Unloading CRO "DllField"
```

Both are `LOG_INFO`, which a release build compiles in and the default log filter passes, so the stock build produces the trace. Source: `src/core/hle/service/ldr_ro/ldr_ro.cpp` in Azahar. Both lines above are from captured runs, shortened to the file name.

The log class prints with a dot, `Service.LDR`, and that is also how `log_filter` spells it. An earlier draft of this note and of the reader wrote it with an underscore, which is the name of the C++ enumerator rather than the name in the log.

## The procedure

Azahar is a Flatpak on the development machine, at version 2126.1.1.

```sh
flatpak run org.azahar_emu.Azahar
```

The first run creates its directories. On the development machine they are:

| | |
| --- | --- |
| Configuration | `~/.var/app/org.azahar_emu.Azahar/config/azahar-emu/qt-config.ini` |
| Log | `~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/log/azahar_log.txt` |

`log_filter` came out as `*:Info`, which is the default and is what the load line needs, so nothing has to be configured. If it has been narrowed, it has to pass `Service.LDR` at `Info` or below.

The Flatpak reaches the filesystem as `host:ro`, so the reference ROM cannot be written to even by accident.

Only one Azahar may run at a time. Two instances share the configuration, the save data and the shader cache, and they interfere with each other instead of failing cleanly. Check with `pgrep -x azahar` before starting a run, and stop a stray one with `pkill -x azahar`, never with a pattern that also matches the shell command doing the killing.

Then three runs, each saved separately, because the log is overwritten. The first boot of a fresh profile is not like the ones after it: it goes through language selection and has no shader cache, so it takes a different path and reaches different modules. Treat the very first run as a rehearsal and keep the three measured runs from a profile that has already booted once.

1. Boot to the title screen and into the overworld, then close.
2. Boot, walk through a door or into a building, which is a field transition, then close.
3. Boot, start a battle, let it finish, then close.

After each one, copy the log somewhere outside the Flatpak's directory. The ROM is read-only input and nothing in this procedure writes to it.

```sh
scripts/read_ldr_ro_trace.py boot.txt field.txt battle.txt
```

## What the answer means

The script reports, for each module, every base address it was seen at, and then says which of the two cases holds.

**If a module ever appears at two addresses**, either within one run or between runs, the question is settled against the optimisation. A cross-module call cannot be lifted as a direct call to a baked-in address, and ADR 0004's decision stops being provisional and becomes the only correct one.

**If every module keeps one address across all three runs**, the optimisation is available: cross-module calls are lifted through the per-module thunk table as the ADR says, and the loader patches the table to direct calls once it has confirmed that each module landed where it expected. The loader asserts the address rather than trusting it, which is the point. Three runs showing stable addresses is evidence that they are usually stable, not a proof that they always are, and the difference is exactly the failure ADR 0004 describes: a wrong address does not crash, it calls a different function and the consequence surfaces somewhere else.

Either way the lifter is written against the position-independent model, which is correct under both answers.

## What else the trace is worth

Two numbers that nothing else in the project can produce.

**How many modules are resident at once**, which the per-module thunk tables have to be sized for and which says how much of the 7.65 MB of module code is mapped at any moment.

**Whether modules are unloaded at all.** The names suggest per-mode loading, `DllField` and `DllBattle` and `DllBox` after the screens they belong to, but nothing has measured it. If they are never unloaded the loader is simpler than it would otherwise have to be.

## What the one captured boot showed

A boot that stopped at language selection, so it is far short of the measurement, but it already establishes things the synthetic self-test cannot.

`DllLangSelect` was mapped at `0x006A5000` with `fixed_end=0x006A9000`, 16 KiB, just above the end of the main executable's static image. The BSS of the static image ends around `0x00662000`, so modules are placed in the address space immediately after it rather than in the heap.

The CRO file itself is a different address. `LoadCRR` reports `crr_buffer_ptr=0x08C5F000`, in the linear heap, which is where the title put the bytes it read from RomFS. The reimplementation has to keep the two apart: the guest buffer holding the file, and the mapping the loader creates for the module.

`cro_helper.cpp` logs more than the ADR asked for, also at `Info`, and it is a free cross-check on our own CRO parsing:

```
CRO "DllLangSelect" exports 0 static anonymous symbols
CRO "DllLangSelect" imports 0 indexed symbols from "|static|"
CRO "DllLangSelect" imports 93 anonymous symbols from "|static|"
CRO "DllLangSelect" exports 0 indexed symbols to "|static|"
CRO "DllLangSelect" exports 1 anonymous symbols to "|static|"
```

Those counts come from the same tables `src/domain/ctr/cro_link_tables.cpp` reads, so a disagreement between the two is a bug in one of them. The section below checks them.

## What the run through the first battle showed

Captured on 2026-10-04 from a fresh profile: language selection, the title screen, the introduction and name entry, the first field scenes, the choice of starter, and the first battle, about seven minutes. The log is kept in the workspace, under `traces/` beside the extraction, because Azahar overwrites its log on the next run.

```sh
scripts/read_ldr_ro_trace.py ~/recomp-data/work/<id>/traces/<run>.txt
```

31 loads and 30 unloads of 15 modules, and never more than three resident at once.

**Four modules moved within the run.** `DllBackGround` was at `0x006C8000`, then `0x006C4000`, then `0x007BB000`. `DllSequence` at `0x006D1000`, then `0x006A5000`. `DllStrInput` at `0x006B5000`, then `0x006A5000`. `DllReport` at `0x007E9000`, then `0x007E5000`. This alone settles the ADR: no second run was needed to compare against.

**Modules are loaded per screen and unloaded when it ends.** The title screen loads `DllTitle`, `DllBackGround` and `DllSequence` and unloads all three before the introduction loads `DllIntro`. The field is `DllField`, mapped at `0x006A5000` with 0x11F000 bytes up to its fixed end, unloaded and reloaded around every scene change: the starter selection is `DllPoke3Select`, loaded at `0x006A5000` after `DllField` goes away, and the battle is `DllBattle` with `DllBackGround`, after which `DllField` comes back at `0x006A5000`. Small helpers such as `DllReport`, `DllFieldEventPlayer` and `DllFieldEventEntranceIn` come and go in seconds while the field stays.

**In this run, the first module after a screen change always got `0x006A5000`,** the bottom of the area above the static image. The others follow it in load order, which is why a module's address depends on what was loaded before it.

**The game chooses the address.** The request of `LoadCRO` carries the address the module is to be mapped at, and the buffers for its data and BSS, chosen by the calling process. Source: 3dbrew, `RO:LoadExeCRO`, request words 2, 4 and 7. Azahar maps the module where it is told and logs the result. The reimplementation will do the same.

The run also logged which services the game reached and which Azahar stubs, among them DSP, FS, FRD, NDM, PTM, APT and `nwm::UDS`. That is input for the service work of a later phase and is not analysed here.

## What the debug level adds

With `log_filter` set to `*:Info Service.LDR:Debug`, `LoadCRO` also logs its request: the address of the file buffer, the address the module is mapped at, the data and BSS buffers with their sizes, the fix level, and which of the two commands was called. A boot to the start menu captured that way on 2026-10-04 shows four loads, every one through the original command, `LoadCRO`, and none through `LoadCRO_New`. Source for the line: `LoadCRO` in Azahar's `ldr_ro.cpp`, which prints `old` or `new`.

The difference between the two commands is a bug the newer one fixes, described in [`formats/cro0.md`](formats/cro0.md) under the relocations read from the reference image, and it cannot affect this title.

## Checking the CRO reader against the trace

`hexlith-cro-dump` prints what Hexlith reads from each module's import tables, and the trace reader compares that with every `cro_helper` line in the logs:

```sh
R=~/recomp-data/work/<id>/romfs
hexlith-cro-dump $R/*.cro $R/static.crs > dump.txt
scripts/read_ldr_ro_trace.py --tables dump.txt ~/recomp-data/work/<id>/traces/*.txt
```

Two things are checked. Every count Azahar logs has to be the count Hexlith reads for the same pair of modules. And every load has to link with exactly the modules Hexlith expects: the ones the new module imports from that are resident at the time, plus the static module, which is always linked, and the resident ones that import from the new module. Which modules are resident comes from the load and unload lines of the same log. Azahar logs a pair only when both modules are present, so the second check is what catches an import module entry read under the wrong name.

Over the three captured runs of 2026-10-04, 184 lines and 36 loads of 16 modules, the static module included, there is no disagreement. That covers the import tables of those 16 modules and nothing else: the other 118 have not been loaded under Azahar yet.
