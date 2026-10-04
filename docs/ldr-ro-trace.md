# Measuring where modules land

[ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) is provisional. It chose to lift CRO modules position-independently, and it named what would settle whether the faster alternative is also safe: a run of the title recording every `ldr:ro` load and unload with the module name and the resulting base address.

This note is how to produce that recording. The measurement itself is not finished. One boot has been captured, which proved the procedure and the reader against a real log, and it holds a single load. The field transition and the battle need somebody to play them.

## No instrumentation is needed

The ADR assumed an instrumented build. It is not necessary. Azahar logs the fact already, in `LoadCRO`:

```
[   3.072226] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO "DllLangSelect" loaded at 0x006A5000, fixed_end=0x006A9000
```

and the matching line when a module goes away:

```
[  45.678901] Service.LDR <Info> ldr_ro.cpp:UnloadCRO:600: Unloading CRO "DllField"
```

Both are `LOG_INFO`, which a release build compiles in and the default log filter passes, so the stock build produces the trace. Source: `src/core/hle/service/ldr_ro/ldr_ro.cpp` in Azahar. The load line above is from the captured boot; the unload line has not been seen yet, and its wording comes from the source.

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

Those counts come from the same tables `src/domain/ctr/cro.cpp` reads, so a disagreement between the two is a bug in one of them.
