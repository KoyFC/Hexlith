#!/usr/bin/env python3
"""Measure how much of what a run executed the analysis tables account for.

hexlith-analyze says which bytes of the main executable are code and where each
function ends. A run of the title says which instructions the game executes.
The static reach (80 percent of the text segment) is a statement about the
bytes; this is the one about the game: of the instructions that ran, how many
sit in a classified code region, and how many in the extent of a function.

An instruction that ran and is in no function extent is a fact the analysis
missed, and the interpreter can always run it, so it costs speed and not
correctness. Grouping those instructions into runs of consecutive addresses
shows what kind of miss each is: a run that starts exactly where a function's
measured extent ends is the rest of that function, code reached by a jump the
walk could not follow.

Inputs are the tables hexlith-analyze writes and the one hexlith-boot
--record-executed writes. All three hold addresses and counts only.

The modules are measured the same way. What a run executed in a module is
recorded as offsets from the module's start, the file offsets the module's
analysis tables use, in observed/modules/<name>/executed.txt, so a module's
tables and its executed table are in one address space whatever address the game
had the module at. With --modules every module with an executed table is
measured against the tables in analysis/modules/<name>/.

Usage:
    scripts/executed_coverage.py --tables ANALYSIS_DIR --executed EXECUTED_TXT
    scripts/executed_coverage.py --modules WORK_DIR/<title>
    scripts/executed_coverage.py --self-test
"""

import argparse
import bisect
import os
import sys
import tempfile


def parse_records(text):
    """Records of a table in the grammar of docs/analysis-tables.md."""
    records = []
    for line in text.splitlines():
        if not line or line.startswith("#") or line.startswith("format "):
            continue
        address, *fields = line.split()
        record = dict(field.split("=", 1) for field in fields)
        record["address"] = int(address, 16)
        records.append(record)
    return records


def extents(records):
    """Sorted (start, end) pairs of records that carry a size."""
    return sorted(
        (r["address"], r["address"] + int(r["size"], 16))
        for r in records
        if int(r["size"], 16) > 0
    )


class Ranges:
    """Membership of an address in a set of sorted, non-overlapping ranges."""

    def __init__(self, pairs):
        self.pairs = sorted(pairs)
        self.starts = [start for start, _ in self.pairs]

    def contains(self, address):
        index = bisect.bisect_right(self.starts, address) - 1
        return index >= 0 and address < self.pairs[index][1]

    def end_of_range_before(self, address):
        """The end of the nearest range that starts at or before the address."""
        index = bisect.bisect_right(self.starts, address) - 1
        return self.pairs[index][1] if index >= 0 else None


def runs_of(addresses, step):
    """Groups sorted addresses into runs of consecutive instructions."""
    runs = []
    for address in addresses:
        if runs and address - runs[-1][1] == step:
            runs[-1][1] = address
        else:
            runs.append([address, address])
    return runs


def measure(functions, regions, executed):
    function_ranges = Ranges(extents(functions))
    code_ranges = Ranges(
        [
            (r["address"], r["address"] + int(r["size"], 16))
            for r in regions
            if r["kind"] == "code"
        ]
    )

    total = sum(int(e["count"]) for e in executed)
    in_code = sum(int(e["count"]) for e in executed if code_ranges.contains(e["address"]))
    in_function = sum(
        int(e["count"]) for e in executed if function_ranges.contains(e["address"])
    )

    outside = [e for e in executed if not function_ranges.contains(e["address"])]
    arm = sorted(e["address"] for e in outside if e["isa"] == "arm")
    thumb = sorted(e["address"] for e in outside if e["isa"] == "thumb")
    weight = {(e["address"], e["isa"]): int(e["count"]) for e in outside}

    runs = [(start, end, "arm") for start, end in runs_of(arm, 4)]
    runs += [(start, end, "thumb") for start, end in runs_of(thumb, 2)]

    after_function = 0
    after_function_weight = 0
    for start, _end, _isa in runs:
        if function_ranges.end_of_range_before(start) == start:
            after_function += 1
            after_function_weight += weight[(start, _isa)]

    functions_executed = {
        function_ranges.pairs[bisect.bisect_right(function_ranges.starts, e["address"]) - 1][0]
        for e in executed
        if function_ranges.contains(e["address"])
    }

    return {
        "executed instructions": total,
        "distinct addresses": len(executed),
        "in a classified code region": in_code,
        "in a function extent": in_function,
        "outside every extent": total - in_function,
        "outside, distinct addresses": len(outside),
        "outside, runs": len(runs),
        "runs starting where a function's extent ends": after_function,
        "functions executed": len(functions_executed),
        "functions known": len(function_ranges.pairs),
    }


def report(figures):
    total = figures["executed instructions"]
    for key, value in figures.items():
        share = ""
        if key in ("in a classified code region", "in a function extent", "outside every extent"):
            share = f"  {100.0 * value / total:6.2f} %"
        print(f"{key:<46}{value:>14,}{share}")


SUMMED = (
    "executed instructions",
    "distinct addresses",
    "in a classified code region",
    "in a function extent",
    "outside every extent",
    "outside, distinct addresses",
    "outside, runs",
    "runs starting where a function's extent ends",
    "functions executed",
    "functions known",
)


def combine(figures_of_modules):
    """The figures of several modules added together."""
    return {key: sum(figures[key] for figures in figures_of_modules) for key in SUMMED}


def measure_modules(extraction):
    """Figures for each module that has an executed table, by the name in its header."""
    observed = os.path.join(extraction, "observed", "modules")
    results = {}
    for name in sorted(os.listdir(observed)):
        executed_path = os.path.join(observed, name, "executed.txt")
        tables = os.path.join(extraction, "analysis", "modules", name)
        if not os.path.isfile(executed_path):
            continue
        with open(executed_path, encoding="utf-8") as stream:
            executed = parse_records(stream.read())
        if not executed:
            continue
        with open(os.path.join(tables, "functions.txt"), encoding="utf-8") as stream:
            functions = parse_records(stream.read())
        with open(os.path.join(tables, "regions.txt"), encoding="utf-8") as stream:
            regions = parse_records(stream.read())
        results[name] = measure(functions, regions, executed)
    return results


def report_modules(results):
    print(f"{'module':<32}{'executed':>14}{'in a function':>16}{'outside':>12}")
    for name, figures in sorted(results.items(), key=lambda item: -item[1]["executed instructions"]):
        total = figures["executed instructions"]
        inside = figures["in a function extent"]
        print(f"{name:<32}{total:>14,}{100.0 * inside / total:>15.2f}%{total - inside:>12,}")
    print()
    report(combine(list(results.values())))


def self_test():
    functions = parse_records(
        "format version=1\n"
        "0x00100000 size=0x00000010 isa=arm source=heuristic\n"
        "0x00100100 size=0x00000008 isa=arm source=heuristic\n"
    )
    regions = parse_records(
        "format version=1\n"
        "0x00100000 size=0x00000010 kind=code isa=arm source=heuristic\n"
        "0x00100010 size=0x00000010 kind=data source=heuristic\n"
    )
    executed = parse_records(
        "format version=1\n"
        "0x00100000 isa=arm count=2\n"
        "0x00100004 isa=arm count=3\n"
        "0x00100010 isa=arm count=5\n"
        "0x00100014 isa=arm count=5\n"
        "0x00100100 isa=arm count=1\n"
        "0x00100200 isa=thumb count=4\n"
    )
    figures = measure(functions, regions, executed)
    assert figures["executed instructions"] == 20, figures
    assert figures["in a classified code region"] == 5, figures
    assert figures["in a function extent"] == 6, figures
    assert figures["outside every extent"] == 14, figures
    assert figures["outside, runs"] == 2, figures
    assert figures["runs starting where a function's extent ends"] == 1, figures
    assert figures["functions executed"] == 2, figures

    combined = combine([figures, figures])
    assert combined["executed instructions"] == 40, combined
    assert combined["functions known"] == 4, combined

    # A module's tables and its executed table are both in offsets from its start.
    with tempfile.TemporaryDirectory() as extraction:
        for directory in ("observed/modules/DllA", "analysis/modules/DllA", "observed/modules/DllB"):
            os.makedirs(os.path.join(extraction, directory))
        files = {
            "analysis/modules/DllA/functions.txt": "format version=1\n"
            "0x00000180 size=0x00000010 isa=arm source=heuristic\n",
            "analysis/modules/DllA/regions.txt": "format version=1\n"
            "0x00000180 size=0x00000010 kind=code isa=arm source=heuristic\n",
            "observed/modules/DllA/executed.txt": "format version=1\n"
            "0x00000180 isa=arm count=3\n0x00000184 isa=arm count=1\n0x00000300 isa=arm count=2\n",
            # A module with an executed table and nothing in it is not measured.
            "observed/modules/DllB/executed.txt": "format version=1\n",
        }
        for path, text in files.items():
            with open(os.path.join(extraction, path), "w", encoding="utf-8") as stream:
                stream.write(text)
        results = measure_modules(extraction)
        assert list(results) == ["DllA"], results
        assert results["DllA"]["executed instructions"] == 6, results
        assert results["DllA"]["in a function extent"] == 4, results
        assert results["DllA"]["outside every extent"] == 2, results
    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tables", help="directory holding functions.txt and regions.txt")
    parser.add_argument("--executed", help="the table hexlith-boot --record-executed wrote")
    parser.add_argument("--modules", help="an extraction directory: measure every module that ran")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.modules:
        report_modules(measure_modules(args.modules))
        return 0
    if not args.tables or not args.executed:
        parser.error("--tables and --executed are required")

    with open(f"{args.tables}/functions.txt", encoding="utf-8") as stream:
        functions = parse_records(stream.read())
    with open(f"{args.tables}/regions.txt", encoding="utf-8") as stream:
        regions = parse_records(stream.read())
    with open(args.executed, encoding="utf-8") as stream:
        executed = parse_records(stream.read())
    report(measure(functions, regions, executed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
