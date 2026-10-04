#!/usr/bin/env python3
"""Read Azahar's log and report what ldr:ro did with each module.

ADR 0004 is provisional and names the measurement that settles it: an
instrumented run of the title recording every load and unload with the module
name and the resulting base address. The instrumentation turned out to be
unnecessary. Azahar logs it already:

    [   12.345678] Service_LDR <Info> ldr_ro.cpp:LoadCRO:531: CRO "DllField" loaded at 0x0A1B2000, fixed_end=0x0A2C3000

That is LOG_INFO, which a release build compiles in, so the stock Flatpak build
produces the trace with nothing more than a log filter.

What this answers is whether a module lands at the same address every time. If it
does, cross-module calls can be lifted as direct calls with the loader asserting
the address it expected. If it does not, the position-independent path is the only
correct one, and ADR 0004 already chose it for that reason.

Usage:
    scripts/read_ldr_ro_trace.py azahar_log.txt
    scripts/read_ldr_ro_trace.py boot.txt field.txt battle.txt
    scripts/read_ldr_ro_trace.py --self-test
"""

import argparse
import collections
import pathlib
import re
import sys

LOADED = re.compile(r'CRO "(?P<name>[^"]*)" loaded at 0x(?P<base>[0-9A-Fa-f]+)'
                    r'(?:, fixed_end=0x(?P<end>[0-9A-Fa-f]+))?')
UNLOADED = re.compile(r'Unloading CRO "(?P<name>[^"]*)"')


class Trace:
    """What one run did, in the order it did it."""

    def __init__(self, name):
        self.name = name
        self.loads = []       # (module, base, end)
        self.unloads = []     # module
        self.bases = collections.defaultdict(set)
        self.load_counts = collections.Counter()
        self.unload_counts = collections.Counter()
        self.peak_resident = 0

    def read(self, text):
        resident = collections.Counter()
        for line in text.splitlines():
            loaded = LOADED.search(line)
            if loaded:
                module = loaded.group("name")
                base = int(loaded.group("base"), 16)
                end = int(loaded.group("end"), 16) if loaded.group("end") else None
                self.loads.append((module, base, end))
                self.bases[module].add(base)
                self.load_counts[module] += 1
                resident[module] += 1
                self.peak_resident = max(self.peak_resident, sum(resident.values()))
                continue

            unloaded = UNLOADED.search(line)
            if unloaded:
                module = unloaded.group("name")
                self.unloads.append(module)
                self.unload_counts[module] += 1
                resident[module] = max(0, resident[module] - 1)

    @property
    def modules(self):
        return set(self.bases)

    def reloaded_at_a_second_address(self):
        return {name: sorted(bases) for name, bases in self.bases.items() if len(bases) > 1}

    def mapped_bytes(self):
        total = 0
        for _, base, end in self.loads:
            if end is not None and end > base:
                total += end - base
        return total


def report(traces):
    """Prints what the traces say, and answers the question ADR 0004 asks."""
    everything = sorted({name for trace in traces for name in trace.modules})

    print(f"traces read            {len(traces)}")
    print(f"modules loaded         {len(everything)}")
    for trace in traces:
        print(f"  {trace.name}: {len(trace.loads)} load(s), {len(trace.unloads)} unload(s), "
              f"at most {trace.peak_resident} resident, {trace.mapped_bytes()} byte(s) mapped")

    # Within one run: a module loaded twice at two addresses settles the question
    # on its own.
    moved_within = {}
    for trace in traces:
        for name, bases in trace.reloaded_at_a_second_address().items():
            moved_within.setdefault(name, []).append((trace.name, bases))

    # Across runs: the same module at a different address in two runs.
    moved_across = {}
    for name in everything:
        seen = {}
        for trace in traces:
            for base in trace.bases.get(name, ()):
                seen.setdefault(base, []).append(trace.name)
        if len(seen) > 1:
            moved_across[name] = seen

    if moved_within:
        print(f"\n{len(moved_within)} module(s) were loaded at more than one address in one run:")
        for name, where in sorted(moved_within.items()):
            for trace_name, bases in where:
                addresses = ", ".join(f"0x{base:08X}" for base in bases)
                print(f"  {name} in {trace_name}: {addresses}")

    if moved_across:
        print(f"\n{len(moved_across)} module(s) were at a different address in different runs:")
        for name, seen in sorted(moved_across.items()):
            for base, runs in sorted(seen.items()):
                print(f"  {name} at 0x{base:08X} in {', '.join(runs)}")

    print()
    if moved_within or moved_across:
        print("A module's base address is not fixed. Lifting a cross-module call as a direct")
        print("call to a baked-in address would be wrong, and ADR 0004's decision stands as")
        print("made rather than as a provisional choice.")
        return 0

    if len(traces) < 2:
        print("Every module kept one address, but one trace cannot show that a second run")
        print("agrees. ADR 0004 asks for a boot, a field transition and a battle.")
        return 0

    print("Every module was loaded at the same address in every run. The optimisation ADR")
    print("0004 describes is available: lift cross-module calls through the thunk table and")
    print("let the loader patch them to direct calls once it has confirmed the addresses.")
    print("The loader asserts the address rather than assuming it, which is the whole point:")
    print("this says the addresses were stable in these runs, not that they always are.")
    return 0


def self_test():
    """The parser, against logs written here rather than taken from a run."""
    fixed = Trace("fixed")
    fixed.read(
        '[   1.0] Service_LDR <Info> ldr_ro.cpp:LoadCRO:531: CRO "DllField" loaded at 0x0A1B2000, fixed_end=0x0A2C3000\n'
        '[   2.0] Service_LDR <Info> ldr_ro.cpp:UnloadCRO:600: Unloading CRO "DllField"\n'
        '[   3.0] Service_LDR <Info> ldr_ro.cpp:LoadCRO:531: CRO "DllBattle" loaded at 0x0A1B2000, fixed_end=0x0A200000\n')
    assert fixed.load_counts["DllField"] == 1, fixed.load_counts
    assert fixed.unload_counts["DllField"] == 1
    assert fixed.bases["DllField"] == {0x0A1B2000}
    assert fixed.peak_resident == 1, fixed.peak_resident
    assert fixed.mapped_bytes() == (0x0A2C3000 - 0x0A1B2000) + (0x0A200000 - 0x0A1B2000)
    assert not fixed.reloaded_at_a_second_address()

    moving = Trace("moving")
    moving.read(
        'CRO "DllField" loaded at 0x0A1B2000\n'
        'Unloading CRO "DllField"\n'
        'CRO "DllField" loaded at 0x0A1C0000\n')
    assert moving.reloaded_at_a_second_address() == {"DllField": [0x0A1B2000, 0x0A1C0000]}
    assert moving.peak_resident == 1

    # A line that is not about loading a module is ignored, including one that
    # merely mentions one.
    quiet = Trace("quiet")
    quiet.read('Service_LDR <Debug> ldr_ro.cpp:LoadCRO:400: called (DllField), cro_buffer_ptr=0x1\n')
    assert not quiet.loads, quiet.loads

    # Two modules resident at once, which is what the question is about.
    both = Trace("both")
    both.read('CRO "A" loaded at 0x1000\nCRO "B" loaded at 0x2000\n')
    assert both.peak_resident == 2

    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", type=pathlib.Path,
                        help="Azahar log files, one per run")
    parser.add_argument("--self-test", action="store_true",
                        help="check the parser against logs written for the purpose")
    options = parser.parse_args()

    if options.self_test:
        return self_test()
    if not options.logs:
        parser.error("name at least one log file")

    traces = []
    for path in options.logs:
        trace = Trace(path.name)
        trace.read(path.read_text(errors="replace"))
        if not trace.loads:
            print(f"{path}: no module loads in it. Is Service_LDR at Info in log_filter?")
        traces.append(trace)

    return report(traces)


if __name__ == "__main__":
    sys.exit(main())
