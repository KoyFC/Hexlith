#!/usr/bin/env python3
"""Compare the golden trace hexlith-boot writes with the one Azahar writes.

A golden trace is the sequence of service requests and CRO loads and unloads a
run makes, one line each, with the frame it happened in:

    # hexlith golden trace, format 1
    0 ipc srv: 00010002
    5 cro-load DllLangSelect 006A5000

hexlith-boot writes one with --golden. Azahar writes the same events into its
log once third_party/azahar/golden-trace.patch is applied, as lines that contain
"golden ipc", "golden cro-load", "golden cro-unload" and "golden vblank", and
`extract` turns that log into a golden trace, counting vertical blanks for the
frame.

`compare` finds the first event where the two disagree and prints it with the
events before it. The comparison is by sequence and not by time: the frame is
reported, never compared, because the two emulators do not take the same time
to do the same thing. By default a run of identical consecutive events counts
as one, since how many times a thread polls before something happens is a
matter of timing. With --per-service each service's requests are compared on
their own, which is what is left when the threads interleave differently.

docs/golden-traces.md describes the format and what a divergence means.

Usage:
    scripts/compare_golden_trace.py extract AZAHAR_LOG > azahar.txt
    scripts/compare_golden_trace.py compare HEXLITH_TRACE AZAHAR_TRACE_OR_LOG
        [--per-service] [--keep-repeats] [--ignore SERVICE ...] [--context N]
        [--until EVENT]
    scripts/compare_golden_trace.py --self-test
"""

import argparse
import collections
import pathlib
import re
import sys

HEADER = "# hexlith golden trace, format 1"

GOLDEN = re.compile(r"golden (?P<event>ipc|cro-load|cro-unload|vblank)(?: (?P<fields>.*))?$")


class Event(collections.namedtuple("Event", "frame kind fields")):
    """One line of a golden trace. fields is what identifies the event: for a
    request the service and the header, for a module its name and address."""

    def key(self):
        return (self.kind,) + self.fields

    def text(self):
        return " ".join((self.kind,) + self.fields)

    def service(self):
        return self.fields[0] if self.kind == "ipc" else "ldr:ro"


def parse_trace(text):
    events = []
    lines = text.splitlines()
    if not lines or lines[0] != HEADER:
        raise ValueError("not a golden trace: the first line is not the format line")
    for number, line in enumerate(lines[1:], start=2):
        if not line or line.startswith("#"):
            continue
        parts = line.split(" ")
        if len(parts) < 3 or not parts[0].isdigit():
            raise ValueError(f"line {number}: '{line}' is not a golden trace event")
        events.append(Event(int(parts[0]), parts[1], tuple(parts[2:])))
    return events


def extract(log):
    """The golden trace in an Azahar log: every golden line in order, each
    event given the number of vertical blanks before it."""
    frame = 0
    lines = [HEADER]
    for line in log.splitlines():
        found = GOLDEN.search(line)
        if found is None:
            continue
        if found["event"] == "vblank":
            frame += 1
            continue
        lines.append(f"{frame} {found['event']} {found['fields']}")
    return "\n".join(lines) + "\n"


def read_either(path):
    text = path.read_text(errors="replace")
    if text.startswith(HEADER):
        return parse_trace(text)
    return parse_trace(extract(text))


def until(events, event_text):
    """The events up to and including the first that reads event_text, such
    as "cro-load DllSequence 006D1000", or all of them if none does."""
    for index, event in enumerate(events):
        if event.text() == event_text:
            return events[:index + 1]
    return events


def normalise(events, keep_repeats, ignored):
    kept = [event for event in events if event.service() not in ignored]
    if keep_repeats:
        return kept
    collapsed = []
    for event in kept:
        if collapsed and collapsed[-1].key() == event.key():
            continue
        collapsed.append(event)
    return collapsed


def first_divergence(ours, theirs):
    for index, (mine, other) in enumerate(zip(ours, theirs)):
        if mine.key() != other.key():
            return index
    if len(ours) != len(theirs):
        return min(len(ours), len(theirs))
    return None


def describe_divergence(name, ours, theirs, context):
    index = first_divergence(ours, theirs)
    if index is None:
        return f"{name}: {len(ours)} events, the same on both sides\n", True
    out = [f"{name}: {len(ours)} events here, {len(theirs)} in Azahar, "
           f"first divergence at event {index}\n"]
    for at in range(max(0, index - context), index + 1):
        mine = ours[at] if at < len(ours) else None
        other = theirs[at] if at < len(theirs) else None
        mark = ">" if at == index else " "
        left = f"{mine.frame:>6} {mine.text()}" if mine else "(ended)"
        right = f"{other.frame:>6} {other.text()}" if other else "(ended)"
        out.append(f"{mark} {at:>7}  {left:<44} | {right}\n")
    return "".join(out), False


def module_frames(events):
    """Frame of each module event, by its text, in order of appearance."""
    frames = collections.defaultdict(list)
    for event in events:
        if event.kind in ("cro-load", "cro-unload"):
            frames[event.text()].append(event.frame)
    return frames


def compare(ours_all, theirs_all, per_service, keep_repeats, ignored, context):
    out = []
    same = True
    if per_service:
        services = sorted({event.service() for event in ours_all + theirs_all} - set(ignored))
        for service in services:
            mine = normalise([e for e in ours_all if e.service() == service], keep_repeats, ())
            other = normalise([e for e in theirs_all if e.service() == service], keep_repeats, ())
            text, agreed = describe_divergence(service, mine, other, context)
            out.append(text)
            same = same and agreed
    else:
        text, same = describe_divergence(
            "all services",
            normalise(ours_all, keep_repeats, ignored),
            normalise(theirs_all, keep_repeats, ignored),
            context)
        out.append(text)

    ours_modules = module_frames(ours_all)
    theirs_modules = module_frames(theirs_all)
    if ours_modules or theirs_modules:
        out.append("\nmodule events, frame here and in Azahar:\n")
        for text in sorted(set(ours_modules) | set(theirs_modules),
                           key=lambda t: (ours_modules.get(t) or theirs_modules.get(t))[0]):
            mine = ours_modules.get(text, [])
            other = theirs_modules.get(text, [])
            for index in range(max(len(mine), len(other))):
                left = str(mine[index]) if index < len(mine) else "-"
                right = str(other[index]) if index < len(other) else "-"
                out.append(f"  {text:<40} {left:>8} {right:>8}\n")
    return "".join(out), same


def self_test():
    log = (
        "[   0.100000] Service <Info> service.cpp:HandleSyncRequest:190: golden ipc srv: 00010002\n"
        "[   0.200000] HW_GPU <Info> gpu.cpp:VBlankCallback:489: golden vblank\n"
        "[   0.300000] Service.LDR <Info> ldr_ro.cpp:LoadCRO:331: CRO \"DllA\" loaded at 0x006A5000\n"
        "[   0.300001] Service.LDR <Info> ldr_ro.cpp:LoadCRO:332: golden cro-load DllA 006A5000\n"
        "[   0.300002] Service <Info> service.cpp:HandleSyncRequest:190: golden ipc ldr:ro 000402C2\n"
        "[   0.400000] HW_GPU <Info> gpu.cpp:VBlankCallback:489: golden vblank\n"
        "[   0.500000] Service <Info> service.cpp:HandleSyncRequest:190: golden ipc gsp::Gpu 000C0000\n"
        "[   0.500001] Service <Info> service.cpp:HandleSyncRequest:190: golden ipc gsp::Gpu 000C0000\n")
    trace = extract(log)
    events = parse_trace(trace)
    assert [e.frame for e in events] == [0, 1, 1, 2, 2], events
    assert events[1] == Event(1, "cro-load", ("DllA", "006A5000")), events[1]

    ours = parse_trace(HEADER + "\n0 ipc srv: 00010002\n3 cro-load DllA 006A5000\n"
                       "3 ipc ldr:ro 000402C2\n4 ipc gsp::Gpu 000C0000\n")
    text, same = compare(ours, events, False, False, (), 2)
    assert same, text
    assert "DllA 006A5000" in text and "       3        1" in text, text

    text, same = compare(ours, events, False, True, (), 2)
    assert not same and "first divergence at event 4" in text, text

    different = parse_trace(HEADER + "\n0 ipc srv: 00010002\n0 ipc srv: 00020000\n")
    text, same = compare(different, events, False, False, (), 1)
    assert not same and "first divergence at event 1" in text, text
    assert "> " in text and "srv: 00020000" in text, text

    text, same = compare(ours, events, True, False, (), 1)
    assert same and "gsp::Gpu: 1 events" in text, text

    cut = until(events, "cro-load DllA 006A5000")
    assert len(cut) == 2 and cut[-1].kind == "cro-load", cut
    assert until(events, "nothing like it") == events

    text, same = compare(ours, events, False, False, ("gsp::Gpu",), 1)
    assert same and "3 events" in text, text

    try:
        parse_trace("0 ipc srv: 00010002\n")
        raise AssertionError("a trace without its format line was accepted")
    except ValueError:
        pass

    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")

    extracting = commands.add_parser("extract", help="turn an Azahar log into a golden trace")
    extracting.add_argument("log", type=pathlib.Path)

    comparing = commands.add_parser("compare", help="find the first divergence")
    comparing.add_argument("ours", type=pathlib.Path)
    comparing.add_argument("theirs", type=pathlib.Path)
    comparing.add_argument("--per-service", action="store_true")
    comparing.add_argument("--keep-repeats", action="store_true")
    comparing.add_argument("--ignore", action="append", default=[])
    comparing.add_argument("--context", type=int, default=8)
    comparing.add_argument("--until", help="stop both traces after this event, "
                                           "for instance 'cro-load DllA 006A5000'")

    options = parser.parse_args()
    if options.self_test:
        return self_test()
    if options.command == "extract":
        sys.stdout.write(extract(options.log.read_text(errors="replace")))
        return 0
    if options.command == "compare":
        ours = read_either(options.ours)
        theirs = read_either(options.theirs)
        if options.until:
            ours = until(ours, options.until)
            theirs = until(theirs, options.until)
        text, same = compare(ours, theirs,
                             options.per_service, options.keep_repeats,
                             tuple(options.ignore), options.context)
        sys.stdout.write(text)
        return 0 if same else 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
