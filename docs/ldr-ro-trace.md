# Measuring where modules land

[ADR 0004](adr/0004-cro-modules-and-the-ldr-ro-hle.md) is provisional. It chose to lift CRO modules position-independently, and it named what would settle whether the faster alternative is also safe: a run of the title recording every `ldr:ro` load and unload with the module name and the resulting base address.

This note is how to produce that recording. The measurement itself has not been made: it needs somebody to play a boot, a field transition and a battle.

## No instrumentation is needed

The ADR assumed an instrumented build. It is not necessary. Azahar logs the fact already, in `LoadCRO`:

```
[   12.345678] Service_LDR <Info> ldr_ro.cpp:LoadCRO:531: CRO "DllField" loaded at 0x0A1B2000, fixed_end=0x0A2C3000
```

and the matching line when a module goes away:

```
[   45.678901] Service_LDR <Info> ldr_ro.cpp:UnloadCRO:600: Unloading CRO "DllField"
```

Both are `LOG_INFO`, which a release build compiles in and the default log filter passes, so the stock build produces the trace. Source: `src/core/hle/service/ldr_ro/ldr_ro.cpp` in Azahar.

## The procedure

Azahar is a Flatpak on the development machine, at version 2126.1.1.

```sh
flatpak run org.azahar_emu.Azahar
```

The first run creates its directories. Check that the log filter still passes `Info`, which is the default: in the configuration file, `log_filter` should be `*:Info` or should name `Service_LDR` at `Info` or below. The file is under `~/.var/app/org.azahar_emu.Azahar/config/`, and this finds it:

```sh
find ~/.var/app/org.azahar_emu.Azahar -name 'qt-config.ini'
find ~/.var/app/org.azahar_emu.Azahar -name '*log*.txt'
```

Then three runs, each saved separately, because the log is overwritten:

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
