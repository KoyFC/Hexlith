#!/usr/bin/env python3
"""Read Azahar's log and report what ldr:ro did with each module.

ADR 0004 is provisional and names the measurement that settles it: an
instrumented run of the title recording every load and unload with the module
name and the resulting base address. The instrumentation turned out to be
unnecessary. Azahar logs it already:

    [   3.072226] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO "DllLangSelect" loaded at 0x006A5000, fixed_end=0x006A9000

That line is from a real run, which is where the format here comes from. It is
LOG_INFO, which a release build compiles in and the default filter passes, so the
stock Flatpak build produces the trace with no configuration at all.

What this answers is whether a module lands at the same address every time. If it
does, cross-module calls can be lifted as direct calls with the loader asserting
the address it expected. If it does not, the position-independent path is the only
correct one, and ADR 0004 already chose it for that reason.

The same log carries a second thing worth having. Beside each load, cro_helper.cpp
logs how many symbols the new module imports from each module it links with, by
index and by position, and how many each module already loaded imports from it:

    CRO "DllField" imports 2817 anonymous symbols from "|static|"
    CRO "DllFieldEventPlayer" exports 30 anonymous symbols to "DllField"

Those counts come from the tables Hexlith's own CRO reader reads, so with
--tables, naming the output of hexlith-cro-dump, every such line is checked against
Hexlith's reading of the same module. So is the set of lines: for each load, the
modules it should have linked with, given what was resident, are the ones logged.

Usage:
    scripts/read_ldr_ro_trace.py azahar_log.txt
    scripts/read_ldr_ro_trace.py boot.txt field.txt battle.txt
    scripts/read_ldr_ro_trace.py --tables dump.txt azahar_log.txt
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
IMPORTS = re.compile(r'CRO "(?P<importer>[^"]*)" imports (?P<count>\d+) '
                     r'(?P<kind>indexed|anonymous) symbols from "(?P<exporter>[^"]*)"')
EXPORTS = re.compile(r'CRO "(?P<exporter>[^"]*)" exports (?P<count>\d+) '
                     r'(?P<kind>indexed|anonymous) symbols to "(?P<importer>[^"]*)"')
STATIC_EXPORTS = re.compile(r'CRO "(?P<module>[^"]*)" exports (?P<count>\d+) '
                            r'static anonymous symbols')

# The static module's name, which the loader treats as always linked.
STATIC = "|static|"

DUMP_MODULE = re.compile(r'^module (?P<name>\S+) .*\bstatic-anonymous=(?P<static>\d+)')
DUMP_IMPORT = re.compile(r'^import (?P<importer>\S+) from=(?P<exporter>\S+) '
                         r'indexed=(?P<indexed>\d+) anonymous=(?P<anonymous>\d+)')


class Tables:
    """What hexlith-cro-dump read from each module's import tables."""

    def __init__(self):
        self.imports = {}         # (importer, exporter) -> {"indexed": n, "anonymous": n}
        self.static_anonymous = {}

    def read(self, text):
        for line in text.splitlines():
            module = DUMP_MODULE.match(line)
            if module:
                self.static_anonymous[module.group("name")] = int(module.group("static"))
                continue
            imported = DUMP_IMPORT.match(line)
            if imported:
                key = (imported.group("importer"), imported.group("exporter"))
                self.imports[key] = {"indexed": int(imported.group("indexed")),
                                     "anonymous": int(imported.group("anonymous"))}

    def exporters_of(self, importer):
        return {exporter for (name, exporter) in self.imports if name == importer}

    def importers_of(self, exporter):
        return {importer for (importer, name) in self.imports if name == exporter}


class LinkCheck:
    """Azahar's link lines against Hexlith's reading of the same modules."""

    def __init__(self, tables):
        self.tables = tables
        self.lines_checked = 0
        self.links_checked = 0
        self.disagreements = []
        self.modules = set()

    def count(self, what, logged, read):
        self.lines_checked += 1
        if read is None:
            self.disagreements.append(f"{what}: Azahar logs {logged}, Hexlith has no such entry")
        elif read != logged:
            self.disagreements.append(f"{what}: Azahar logs {logged}, Hexlith reads {read}")

    def line(self, text):
        """Checks one link line and returns the pair it is about, or None."""
        imported = IMPORTS.search(text)
        if imported:
            importer, exporter = imported.group("importer"), imported.group("exporter")
            entry = self.tables.imports.get((importer, exporter))
            kind = imported.group("kind")
            self.count(f"{importer} imports {kind} from {exporter}",
                       int(imported.group("count")), entry[kind] if entry else None)
            return ("imports", importer, exporter)

        exported = EXPORTS.search(text)
        if exported:
            importer, exporter = exported.group("importer"), exported.group("exporter")
            entry = self.tables.imports.get((importer, exporter))
            kind = exported.group("kind")
            self.count(f"{exporter} exports {kind} to {importer}",
                       int(exported.group("count")), entry[kind] if entry else None)
            return ("exports", importer, exporter)

        static = STATIC_EXPORTS.search(text)
        if static:
            module = static.group("module")
            self.count(f"{module} exports static anonymous symbols",
                       int(static.group("count")), self.tables.static_anonymous.get(module))
        return None

    def load(self, module, resident, logged):
        """Checks that a load linked with exactly the modules it should have."""
        self.links_checked += 1
        self.modules.add(module)
        linked = resident | {STATIC}
        expected = set()
        for exporter in self.tables.exporters_of(module) & linked:
            expected.add(("imports", module, exporter))
        for importer in self.tables.importers_of(module) & linked:
            expected.add(("exports", importer, module))
        for missing in sorted(expected - logged):
            self.disagreements.append(f"loading {module}: Hexlith expects a link {missing} "
                                      f"that Azahar did not log")
        for extra in sorted(logged - expected):
            self.disagreements.append(f"loading {module}: Azahar logged a link {extra} "
                                      f"that Hexlith does not expect")

    def read(self, text):
        resident = set()
        pending = set()
        for line in text.splitlines():
            pair = self.line(line)
            if pair:
                pending.add(pair)
                continue
            loaded = LOADED.search(line)
            if loaded:
                module = loaded.group("name")
                self.load(module, resident, pending)
                resident.add(module)
                pending = set()
                continue
            unloaded = UNLOADED.search(line)
            if unloaded:
                resident.discard(unloaded.group("name"))
                pending = set()

    def report(self):
        print(f"\nlink lines checked     {self.lines_checked}")
        print(f"loads checked          {self.links_checked}, of {len(self.modules)} module(s)")
        if not self.disagreements:
            print("Every count Azahar logged is the count Hexlith reads, and every load linked")
            print("with the modules Hexlith expects.")
            return 0
        print(f"{len(self.disagreements)} disagreement(s):")
        for disagreement in self.disagreements:
            print(f"  {disagreement}")
        return 1


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

    # Across runs: a module whose addresses in one run are not its addresses in
    # another. With a single run there is nothing to compare, and a module that
    # moved within that run is reported above, not here.
    moved_across = {}
    for name in everything:
        per_run = [frozenset(trace.bases[name]) for trace in traces if name in trace.bases]
        if len(per_run) > 1 and len(set(per_run)) > 1:
            seen = {}
            for trace in traces:
                for base in trace.bases.get(name, ()):
                    seen.setdefault(base, []).append(trace.name)
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
        '[   1.0] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO "DllField" loaded at 0x0A1B2000, fixed_end=0x0A2C3000\n'
        '[   2.0] Service.LDR <Info> ldr_ro.cpp:UnloadCRO:600: Unloading CRO "DllField"\n'
        '[   3.0] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO "DllBattle" loaded at 0x0A1B2000, fixed_end=0x0A200000\n')
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
    quiet.read('Service.LDR <Debug> ldr_ro.cpp:LoadCRO:300: called (DllField), cro_buffer_ptr=0x1\n')
    assert not quiet.loads, quiet.loads

    # The import and export counts cro_helper.cpp logs beside each load are not
    # loads, however much they look like one.
    counted = Trace("counted")
    counted.read(
        '[   3.072159] Service.LDR <Info> cro_helper.cpp:operator():886: CRO "DllLangSelect" imports 93 anonymous symbols from "|static|"\n')
    assert not counted.loads, counted.loads

    # Two modules resident at once, which is what the question is about.
    both = Trace("both")
    both.read('CRO "A" loaded at 0x1000\nCRO "B" loaded at 0x2000\n')
    assert both.peak_resident == 2

    tables = Tables()
    tables.read(
        "module |static| segments=6 static-anonymous=0\n"
        "import |static| from=DllA indexed=0 anonymous=2\n"
        "module DllA segments=6 static-anonymous=0\n"
        "import DllA from=|static| indexed=0 anonymous=93\n"
        "module DllB segments=6 static-anonymous=0\n"
        "import DllB from=|static| indexed=0 anonymous=7\n"
        "import DllB from=DllA indexed=0 anonymous=4\n"
        "import DllB from=DllC indexed=0 anonymous=1\n")
    assert tables.imports[("DllB", "DllA")] == {"indexed": 0, "anonymous": 4}
    assert tables.exporters_of("DllB") == {STATIC, "DllA", "DllC"}
    assert tables.importers_of("DllA") == {STATIC, "DllB"}

    # A load of A, then of B while A is resident. B imports from C too, which is
    # never loaded, so no line is expected for it.
    agreeing = LinkCheck(tables)
    agreeing.read(
        'CRO "DllA" exports 0 static anonymous symbols\n'
        'CRO "DllA" imports 0 indexed symbols from "|static|"\n'
        'CRO "DllA" imports 93 anonymous symbols from "|static|"\n'
        'CRO "DllA" exports 0 indexed symbols to "|static|"\n'
        'CRO "DllA" exports 2 anonymous symbols to "|static|"\n'
        'CRO "DllA" loaded at 0x006A5000\n'
        'CRO "DllB" imports 7 anonymous symbols from "|static|"\n'
        'CRO "DllB" imports 4 anonymous symbols from "DllA"\n'
        'CRO "DllB" loaded at 0x006A9000\n')
    assert not agreeing.disagreements, agreeing.disagreements
    assert agreeing.lines_checked == 7, agreeing.lines_checked
    assert agreeing.links_checked == 2

    # A count that differs, and a link Azahar made that the tables do not have.
    disagreeing = LinkCheck(tables)
    disagreeing.read(
        'CRO "DllA" imports 92 anonymous symbols from "|static|"\n'
        'CRO "DllA" exports 2 anonymous symbols to "|static|"\n'
        'CRO "DllA" imports 1 anonymous symbols from "DllZ"\n'
        'CRO "DllA" loaded at 0x006A5000\n')
    assert len(disagreeing.disagreements) == 3, disagreeing.disagreements

    # B loaded after A went away must not link with A.
    unloaded = LinkCheck(tables)
    unloaded.read(
        'CRO "DllA" imports 93 anonymous symbols from "|static|"\n'
        'CRO "DllA" exports 2 anonymous symbols to "|static|"\n'
        'CRO "DllA" loaded at 0x006A5000\n'
        'Unloading CRO "DllA"\n'
        'CRO "DllB" imports 7 anonymous symbols from "|static|"\n'
        'CRO "DllB" loaded at 0x006A5000\n')
    assert not unloaded.disagreements, unloaded.disagreements

    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", type=pathlib.Path,
                        help="Azahar log files, one per run")
    parser.add_argument("--tables", type=pathlib.Path,
                        help="the output of hexlith-cro-dump over the title's modules, to check "
                             "the link lines against")
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
            print(f"{path}: no module loads in it. Is Service.LDR at Info in log_filter?")
        traces.append(trace)

    status = report(traces)
    if options.tables is None:
        return status

    tables = Tables()
    tables.read(options.tables.read_text())
    check = LinkCheck(tables)
    for path in options.logs:
        check.read(path.read_text(errors="replace"))
    return check.report() or status


if __name__ == "__main__":
    sys.exit(main())
