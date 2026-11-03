#!/usr/bin/env python3
"""Measure what showing the title in a window costs, stretch by stretch.

Phase 10 asks how close the window comes to the speed of the run without one.
The question is asked of the stretches of a boot and not of the whole of it,
because the first 227 frames are almost all drawing, while what comes after the
language prompt is mostly the game's own code, and one figure for the whole
boot would hide which of the two a change helped.

Each configuration boots from a new game to the last frame given, with
hexlith-boot printing the wall time and the game's time at every frame asked
for (--time-frames). The configurations are run alternately, several times each,
on the performance cores, waiting for the machine to settle before each run, as
scripts/measure_lift_speed.py does. The game runs on one thread, but the Vulkan
driver and llvmpipe have threads of their own, so pinning to a single core would
measure a machine nobody draws on. Every run must execute the same number of
instructions up to each frame; one that does not is not the same work and the
script stops. What is reported is the median of each stretch.

Where a stretch's game time alone is longer than the console takes for it, the
window cannot be blamed for the shortfall, so the table shows that real time.

docs/gpu-speed.md describes the protocol and records what it measured.

Usage:
    scripts/measure_window_speed.py --boot BOOT --lifted LIBRARY --rom-dir ROM
        --work-dir WORK --input SCRIPT [--frames 227,400,700,1000,1101]
        [--configs none,intel,llvmpipe] [--limit N] [--runs 5] [--cores LIST]
        [--max-load 1.0] [--dummy-video]
    scripts/measure_window_speed.py --self-test
"""

import argparse
import os
import re
import statistics
import subprocess
import sys

import measure_lift_speed

TIMING = re.compile(
    r"^timing\s+frame (\d+) wall ([0-9.]+) s guest ([0-9.]+) s instructions (\d+)$", re.MULTILINE)
TOTAL = re.compile(r"^(\d+) instructions, (\d+) ticks, in ([0-9.]+) s", re.MULTILINE)
LISTS = re.compile(
    r"^display lists (\d+) in ([0-9.]+) s, transfers (\d+) in ([0-9.]+) s, "
    r"present ([0-9.]+) s$", re.MULTILINE)
BACKEND = re.compile(
    r"^backend (\d+) draws in (\d+) groups, (\d+) bytes up and (\d+) down, (\d+) buffers made$",
    re.MULTILINE)
PARTS = re.compile(
    r"^\s+textures ([0-9.]+) s, up ([0-9.]+) s, down ([0-9.]+) s, device ([0-9.]+) s, "
    r"buffers ([0-9.]+) s$", re.MULTILINE)

# The console's vertical blank rate, which sets the speed a stretch is held to.
FRAMES_PER_SECOND = 59.83

# What a configuration adds to the command line. "none" is the floor: the same
# boot without a window.
CONFIGS = {
    "none": [],
    "intel": ["--window", "--backend", "vulkan", "--device", "Intel"],
    "llvmpipe": ["--window", "--backend", "vulkan", "--device", "llvmpipe"],
    "reference": ["--window", "--backend", "reference"],
}

# The numbers of the display lines, by name, in the order the lines print them.
DISPLAY_PARTS = ("lists", "lists_s", "transfers", "transfers_s", "present_s", "draws", "groups",
                 "up_bytes", "down_bytes", "buffers_made", "textures_s", "up_s", "down_s",
                 "device_s", "buffers_s")


def parse_run(text):
    """What one boot printed: the timeline by frame, the totals and the display
    work, which is empty for a run without a window."""
    timeline = {}
    for frame, wall, guest, instructions in TIMING.findall(text):
        timeline[int(frame)] = (float(wall), float(guest), int(instructions))
    total = TOTAL.search(text)
    if not timeline or total is None:
        raise ValueError("the boot did not print its timeline and its totals")
    display = {}
    lists, backend, parts = LISTS.search(text), BACKEND.search(text), PARTS.search(text)
    if lists and backend and parts:
        numbers = [float(value) for match in (lists, backend, parts) for value in match.groups()]
        display = dict(zip(DISPLAY_PARTS, numbers))
    return {"timeline": timeline, "instructions": int(total.group(1)), "ticks": int(total.group(2)),
            "seconds": float(total.group(3)), "display": display}


def stretches(frames):
    """The stretches between the frames given, the first from the start of the
    boot, as pairs of the frame it starts after and the frame it ends at."""
    return list(zip([0] + list(frames[:-1]), frames))


def stretch_times(run, frames):
    """Wall and guest seconds of each stretch of one run."""
    times = []
    before = (0.0, 0.0)
    for start, end in stretches(frames):
        wall, guest, _ = run["timeline"][end]
        times.append((start, end, wall - before[0], guest - before[1]))
        before = (wall, guest)
    return times


def check_same_work(runs, frames):
    """Every run has to have executed the same instructions by each frame."""
    first = runs[0]
    for run in runs[1:]:
        for frame in frames:
            if run["timeline"][frame][2] != first["timeline"][frame][2]:
                raise ValueError(f"frame {frame} took {run['timeline'][frame][2]} instructions "
                                 f"in one run and {first['timeline'][frame][2]} in another")
        if run["ticks"] != first["ticks"]:
            raise ValueError("the runs did not end on the same tick")


def summarise(runs_by_config, frames):
    """Medians of each stretch's wall and guest time and of the display work,
    for each configuration."""
    summary = {}
    for name, runs in runs_by_config.items():
        per_stretch = [stretch_times(run, frames) for run in runs]
        medians = []
        for index, (start, end) in enumerate(stretches(frames)):
            medians.append({
                "from": start, "to": end,
                "wall": statistics.median(times[index][2] for times in per_stretch),
                "guest": statistics.median(times[index][3] for times in per_stretch)})
        display = {}
        if runs[0]["display"]:
            for part in DISPLAY_PARTS:
                display[part] = statistics.median(run["display"][part] for run in runs)
        summary[name] = {"stretches": medians, "display": display,
                         "total": statistics.median(run["seconds"] for run in runs)}
    return summary


def overhead(window, floor):
    """How much longer than the floor, as a fraction of the floor."""
    return (window - floor) / floor if floor > 0 else float("inf")


def boot_command(arguments, config):
    command = [arguments.boot, "--rom-dir", arguments.rom_dir, "--work-dir", arguments.work_dir,
               "--new-game", "--input", arguments.input, "--lifted", arguments.lifted,
               "--limit", str(arguments.limit),
               "--time-frames", ",".join(str(frame) for frame in arguments.frames),
               "--stop-after", f"frame:{arguments.frames[-1]}"]
    return command + CONFIGS[config]


def run_boot(arguments, config):
    command = boot_command(arguments, config)
    environment = dict(os.environ)
    if arguments.dummy_video:
        environment["SDL_VIDEODRIVER"] = "dummy"
    cores = set(arguments.cores)
    pin = lambda: os.sched_setaffinity(0, cores)
    finished = subprocess.run(command, capture_output=True, text=True, check=True,
                              preexec_fn=pin, env=environment)
    return parse_run(finished.stdout)


def measure(arguments):
    runs = {name: [] for name in arguments.configs}
    for round_number in range(arguments.runs):
        for name in arguments.configs:
            measure_lift_speed.wait_for_quiet(arguments.max_load)
            load = os.getloadavg()[0]
            run = run_boot(arguments, name)
            runs[name].append(run)
            print(f"round {round_number + 1}: {name:9} {run['seconds']:.2f} s, "
                  f"{run['instructions']} instructions, load {load:.2f} before", flush=True)
    every_run = [run for config_runs in runs.values() for run in config_runs]
    try:
        check_same_work(every_run, arguments.frames)
    except ValueError as problem:
        sys.exit(f"{problem}; nothing to compare")
    print_summary(summarise(runs, arguments.frames), arguments.configs)


def print_summary(summary, configs):
    floor = summary.get("none")
    print("\nstretch                  real time  " + "  ".join(f"{name:>16}" for name in configs))
    count = len(summary[configs[0]]["stretches"])
    for index in range(count):
        first = summary[configs[0]]["stretches"][index]
        real = (first["to"] - first["from"]) / FRAMES_PER_SECOND
        cells = []
        for name in configs:
            wall = summary[name]["stretches"][index]["wall"]
            text = f"{wall:7.2f} s"
            if floor is not None and name != "none":
                text += f" {overhead(wall, floor['stretches'][index]['wall']):+6.0%}"
            cells.append(f"{text:>16}")
        guest = first["guest"]
        print(f"frames {first['from']:>4} to {first['to']:>4}  {real:7.2f} s  " + "  ".join(cells)
              + f"   (game alone {guest:.2f} s)")
    print()
    for name in configs:
        display = summary[name]["display"]
        if not display:
            continue
        print(f"{name}: total {summary[name]['total']:.2f} s; lists {display['lists_s']:.2f} s "
              f"(textures {display['textures_s']:.2f}, up {display['up_s']:.2f}, "
              f"down {display['down_s']:.2f}, device {display['device_s']:.2f}, "
              f"buffers {display['buffers_s']:.2f}), transfers {display['transfers_s']:.2f} s, "
              f"present {display['present_s']:.2f} s; {int(display['groups'])} groups, "
              f"{display['up_bytes'] / 1e6:.0f} MB up, {display['down_bytes'] / 1e6:.0f} MB down")


def performance_cores():
    """The cores a hybrid processor's performance cores are, or all that are allowed."""
    allowed = os.sched_getaffinity(0)
    try:
        with open(measure_lift_speed.PERFORMANCE_CORES) as listing:
            performance = [cpu for cpu in measure_lift_speed.parse_cpu_list(listing.read())
                           if cpu in allowed]
        if performance:
            return performance
    except OSError:
        pass
    return sorted(allowed)


def parse_frames(text):
    frames = sorted({int(part) for part in text.split(",")})
    if not frames or frames[0] <= 0:
        raise argparse.ArgumentTypeError("frames are positive numbers")
    return frames


def self_test():
    sample = ("timing  frame 227 wall 8.790 s guest 0.562 s instructions 41972784\n"
              "timing  frame 400 wall 20.000 s guest 5.000 s instructions 400000000\n"
              "display lists 1153 in 6.040 s, transfers 445 in 0.722 s, present 1.187 s\n"
              "backend 15673 draws in 478 groups, 501219328 bytes up and 501219328 down, "
              "2390 buffers made\n"
              "        textures 0.668 s, up 1.128 s, down 1.167 s, device 2.346 s, buffers 0.126 s\n"
              "41972790 instructions, 1017217878 ticks, in 8.79 s: 4.8 million a second\n")
    run = parse_run(sample)
    assert run["timeline"][400] == (20.0, 5.0, 400000000), run
    assert run["display"]["groups"] == 478 and run["display"]["device_s"] == 2.346, run
    assert run["ticks"] == 1017217878 and run["seconds"] == 8.79, run
    plain = parse_run(sample.split("display")[0] + sample.split("\n")[-2] + "\n")
    assert plain["display"] == {}, plain
    try:
        parse_run("nothing")
        raise AssertionError("output with no timeline was accepted")
    except ValueError:
        pass
    assert stretches([227, 400, 1101]) == [(0, 227), (227, 400), (400, 1101)]
    times = stretch_times(run, [227, 400])
    assert times[0] == (0, 227, 8.79, 0.562), times
    assert abs(times[1][2] - 11.21) < 1e-9 and abs(times[1][3] - 4.438) < 1e-9, times
    other = parse_run(sample)
    check_same_work([run, other], [227, 400])
    other["timeline"][400] = (20.0, 5.0, 400000001)
    try:
        check_same_work([run, other], [227, 400])
        raise AssertionError("runs of different work were accepted")
    except ValueError:
        pass
    slower = parse_run(sample)
    slower["timeline"][227] = (10.0, 0.6, 41972784)
    slower["timeline"][400] = (30.0, 5.0, 400000000)
    summary = summarise({"x": [run, slower, run]}, [227, 400])
    assert summary["x"]["stretches"][0]["wall"] == 8.79, summary
    assert abs(overhead(11.0, 10.0) - 0.1) < 1e-9
    arguments = argparse.Namespace(boot="boot", rom_dir="rom", work_dir="work", input="in",
                                   lifted="lib", limit=20000000000, frames=[227, 1101])
    command = boot_command(arguments, "intel")
    assert command[command.index("--time-frames") + 1] == "227,1101", command
    assert command[command.index("--stop-after") + 1] == "frame:1101", command
    assert command[command.index("--device") + 1] == "Intel", command
    assert "--window" not in boot_command(arguments, "none")
    assert parse_frames("400,227,400") == [227, 400]
    print("self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--boot")
    parser.add_argument("--lifted")
    parser.add_argument("--rom-dir")
    parser.add_argument("--work-dir")
    parser.add_argument("--input")
    parser.add_argument("--frames", type=parse_frames, default=parse_frames("227,400,700,1000,1101"),
                        help="the frames to time; the last one is where the boot stops")
    parser.add_argument("--configs", default="none,intel",
                        help="comma separated, of " + ", ".join(CONFIGS))
    parser.add_argument("--limit", type=int, default=20000000000,
                        help="the instruction limit; the default of hexlith-boot stops a boot "
                             "to frame 1101 early")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cores", type=measure_lift_speed.parse_cpu_list,
                        help="the cores to run on, such as 0-3; the performance cores by default")
    parser.add_argument("--max-load", type=float, default=1.0)
    parser.add_argument("--dummy-video", action="store_true",
                        help="open no real window (SDL_VIDEODRIVER=dummy), to leave out the "
                             "desktop's part of presenting")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return
    required = ("boot", "lifted", "rom_dir", "work_dir", "input")
    missing = [name for name in required if getattr(arguments, name) is None]
    if missing:
        parser.error("missing " + ", ".join("--" + name.replace("_", "-") for name in missing))
    arguments.configs = arguments.configs.split(",")
    unknown = [name for name in arguments.configs if name not in CONFIGS]
    if unknown:
        parser.error("unknown configuration " + ", ".join(unknown))
    if arguments.cores is None:
        arguments.cores = performance_cores()
    print(f"measuring on cores {arguments.cores}", flush=True)
    measure(arguments)


if __name__ == "__main__":
    main()
