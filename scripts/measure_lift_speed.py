#!/usr/bin/env python3
"""Measure how much faster lifted code runs the title than the interpreter.

Phase 8 stops if lifted code runs the game's own code less than five times
faster than the interpreter. One run of each is not enough to say: the time a
boot to the title spends in the game's code is under a second with lifted code,
and a busy machine or a core changing clock moves it by a tenth. So the two are
run alternately, several times each, on one core of a machine that is waiting
for nothing else, and the medians are compared.

What is compared is the time hexlith-boot reports on its "guest" line: the time
spent running the game's instructions, apart from system calls, services and
the start-up. Every run has to execute the same number of instructions; one
that does not is not a measurement of the same work, and the script stops.

docs/lifter.md describes the protocol and records what it measured.

Usage:
    scripts/measure_lift_speed.py --boot BOOT --lifted LIBRARY --rom-dir ROM
        --work-dir WORK --input SCRIPT [--frame 227] [--limit N] [--runs 7] [--core N]
        [--max-load 1.0]
    scripts/measure_lift_speed.py --self-test
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

GUEST = re.compile(r"^guest\s+([0-9.]+) s running the game's code", re.MULTILINE)
COUNT = re.compile(r"^(\d+) instructions, (\d+) ticks", re.MULTILINE)

# Where Linux lists a hybrid processor's performance cores.
PERFORMANCE_CORES = "/sys/devices/cpu_core/cpus"

# How long to wait for the machine to settle before a run, in seconds.
SETTLE_LIMIT = 300
SETTLE_POLL = 5


def parse_run(text):
    """The guest seconds and the instruction count a boot printed."""
    guest = GUEST.search(text)
    count = COUNT.search(text)
    if guest is None or count is None:
        raise ValueError("the boot did not print its guest time and instruction count")
    return float(guest.group(1)), int(count.group(1))


def summarise(interpreted, lifted):
    """Medians, spreads and the ratio of the medians, from lists of seconds."""
    middle_interpreted = statistics.median(interpreted)
    middle_lifted = statistics.median(lifted)
    return {
        "interpreter": (middle_interpreted, min(interpreted), max(interpreted)),
        "lifted": (middle_lifted, min(lifted), max(lifted)),
        "ratio": middle_interpreted / middle_lifted,
    }


def parse_cpu_list(text):
    """The CPUs of a list in the kernel's format, such as 0-3,6."""
    cpus = []
    for part in text.strip().split(","):
        first, _, last = part.partition("-")
        cpus.extend(range(int(first), int(last or first) + 1))
    return cpus


def default_core():
    """A performance core to run on, the last one so as to leave the first,
    which takes more interrupts. The two kinds of core of a hybrid processor
    run the two builds at different speeds and at a different ratio, so the
    measurement is made on one kind and says which core it used."""
    allowed = os.sched_getaffinity(0)
    try:
        with open(PERFORMANCE_CORES) as listing:
            performance = [cpu for cpu in parse_cpu_list(listing.read()) if cpu in allowed]
        if performance:
            return max(performance)
    except OSError:
        pass
    return max(allowed)


def wait_for_quiet(max_load):
    """Waits until the one-minute load average is below max_load."""
    waited = 0
    while os.getloadavg()[0] >= max_load:
        if waited >= SETTLE_LIMIT:
            print(f"warning: load still {os.getloadavg()[0]:.2f} after {waited} s, measuring anyway",
                  file=sys.stderr)
            return
        time.sleep(SETTLE_POLL)
        waited += SETTLE_POLL


def boot_command(arguments, lifted):
    """The command line of one boot. A run to frame 1101 takes 3.87 billion instructions and
    hexlith-boot stops at two billion unless it is given a limit."""
    command = [arguments.boot, "--rom-dir", arguments.rom_dir, "--work-dir", arguments.work_dir,
               "--new-game", "--input", arguments.input]
    if arguments.limit is not None:
        command += ["--limit", str(arguments.limit)]
    command += ["--stop-after", f"frame:{arguments.frame}"]
    if lifted:
        command += ["--lifted", arguments.lifted]
    return command


def run_boot(arguments, core, lifted):
    command = boot_command(arguments, lifted)
    pin = (lambda: os.sched_setaffinity(0, {core})) if core is not None else None
    finished = subprocess.run(command, capture_output=True, text=True, check=True, preexec_fn=pin)
    return parse_run(finished.stdout)


def measure(arguments):
    seconds = {False: [], True: []}
    counts = set()
    for round_number in range(arguments.runs):
        for lifted in (False, True):
            wait_for_quiet(arguments.max_load)
            load = os.getloadavg()[0]
            guest, count = run_boot(arguments, arguments.core, lifted)
            seconds[lifted].append(guest)
            counts.add(count)
            print(f"round {round_number + 1}: {'lifted     ' if lifted else 'interpreter'} "
                  f"{guest:.3f} s, {count} instructions, load {load:.2f} before", flush=True)
            if len(counts) != 1:
                sys.exit("the runs did not execute the same instructions; nothing to compare")
    summary = summarise(seconds[False], seconds[True])
    for name in ("interpreter", "lifted"):
        middle, low, high = summary[name]
        print(f"{name:11} median {middle:.3f} s, from {low:.3f} to {high:.3f} s")
    print(f"lifted code is {summary['ratio']:.2f} times faster, median to median")


def self_test():
    sample = ("thread 4 at 00173024\n"
              "41972790 instructions, 1017217878 ticks, in 3.10 s: 13.5 million a second\n"
              "guest   0.73 s running the game's code: 57.3 million instructions a second\n")
    assert parse_run(sample) == (0.73, 41972790)
    try:
        parse_run("nothing")
        raise AssertionError("output with no guest line was accepted")
    except ValueError:
        pass
    summary = summarise([3.0, 2.8, 3.4], [0.6, 0.7, 0.5])
    assert summary["interpreter"] == (3.0, 2.8, 3.4)
    assert summary["lifted"] == (0.6, 0.5, 0.7)
    assert abs(summary["ratio"] - 5.0) < 1e-9
    arguments = argparse.Namespace(boot="boot", rom_dir="rom", work_dir="work", input="in",
                                   frame=1101, limit=None, lifted="lib")
    plain = boot_command(arguments, False)
    assert "--limit" not in plain, plain
    assert "--lifted" not in plain, plain
    assert plain[-2:] == ["--stop-after", "frame:1101"], plain
    arguments.limit = 20000000000
    limited = boot_command(arguments, True)
    assert limited[limited.index("--limit") + 1] == "20000000000", limited
    assert limited[limited.index("--lifted") + 1] == "lib", limited
    assert parse_cpu_list("0-3\n") == [0, 1, 2, 3]
    assert parse_cpu_list("0-1,6") == [0, 1, 6]
    print("self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--boot")
    parser.add_argument("--lifted")
    parser.add_argument("--rom-dir")
    parser.add_argument("--work-dir")
    parser.add_argument("--input")
    parser.add_argument("--frame", type=int, default=227)
    parser.add_argument("--limit", type=int,
                        help="the instruction limit to give hexlith-boot, whose default of two "
                             "billion stops a run to frame 1101 early")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--core", type=int,
                        help="the core to run on; the last performance core by default")
    parser.add_argument("--max-load", type=float, default=1.0,
                        help="the one-minute load average to wait for; a desktop idles near 1")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return
    required = ("boot", "lifted", "rom_dir", "work_dir", "input")
    missing = [name for name in required if getattr(arguments, name) is None]
    if missing:
        parser.error("missing " + ", ".join("--" + name.replace("_", "-") for name in missing))
    if arguments.core is None:
        arguments.core = default_core()
    print(f"measuring on core {arguments.core}", flush=True)
    measure(arguments)


if __name__ == "__main__":
    main()
