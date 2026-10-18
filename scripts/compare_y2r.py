#!/usr/bin/env python3
"""Compare Hexlith's YUV to RGB conversion with Azahar's.

y2r:u converts the frames of the videos the title plays, and a conversion
that is off by one in a channel, or puts a block in the wrong place, shows on
screen as a tint or a tear that nothing else would explain. So the conversion
is run against Azahar's, whose code says it matches hardware bit for bit as
far as it was tested, on random cases: every input and output format, every
rotation, both layouts, widths and heights that end mid-strip, random
coefficients as well as the standard sets, random samples.

Both sides read the same file of cases. Azahar runs them through
HW::Y2R::PerformConversion at its first vertical blank, under the oracle hooks
of third_party/azahar/oracle.patch, and quits; Hexlith runs them through
video::ConvertYuvToRgb with tools/hexlith-oracle-run. The outputs are compared
byte for byte.

Azahar needs a title to boot before it reaches a vertical blank, so it is
given the cartridge image. It runs with its configuration and data in a
directory of its own, so nothing of a person's Azahar profile is touched.

Usage:
    scripts/compare_y2r.py --azahar ~/opt/src/azahar/build/bin/Release/azahar \\
        --image ~/recomp-data/rom/pokemon-x.3ds \\
        --tool build/release/tools/hexlith-oracle-run/hexlith-oracle-run \\
        --cases 2000 --work /tmp/y2r
"""

import argparse
import collections
import os
import pathlib
import random
import struct
import subprocess
import sys

# The standard coefficient sets of y2r:u: ITU Rec. 601 and 709, with and
# without scaling. Source: standard_coefficients in Azahar's y2r_u.cpp.
STANDARD_SETS = [
    [0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B],
    [0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51],
    [0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B],
    [0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421],
]

INPUT_NAMES = ["422", "420", "422 wide", "420 wide", "YUYV"]
OUTPUT_NAMES = ["RGBA8", "RGB8", "RGB5A1", "RGB565"]


def plane_sizes(input_format, width, lines):
    """Bytes of Y, U, V and YUYV as they sit in memory."""
    pixels = width * lines
    if input_format == 4:
        return [0, 0, 0, pixels * 2]
    chroma = pixels // 4 if input_format in (1, 3) else pixels // 2
    sample = 2 if input_format in (2, 3) else 1
    return [pixels * sample, chroma * sample, chroma * sample, 0]


def random_case(rng):
    input_format = rng.randrange(5)
    output_format = rng.randrange(4)
    rotation = rng.randrange(4)
    layout = rng.randrange(2)
    width = 8 * rng.randint(1, 8)
    lines = 8 * rng.randint(1, 3) if layout == 1 else rng.randint(1, 24)
    if input_format in (1, 3) and lines % 2:
        # Hexlith refuses an odd count in 4:2:0, where the last line would
        # need chroma the hardware is never sent.
        lines += 1
    kind = rng.random()
    if kind < 0.3:
        coefficients = list(rng.choice(STANDARD_SETS))
    elif kind < 0.7:
        coefficients = [rng.randrange(0x400) for _ in range(5)] + \
                       [rng.randint(-0x8000, 0x7FFF) for _ in range(3)]
    else:
        coefficients = [rng.randint(-0x8000, 0x7FFF) for _ in range(8)]
    alpha = rng.randrange(256)
    planes = [bytes(rng.randrange(256) for _ in range(size))
              for size in plane_sizes(input_format, width, lines)]
    return (input_format, output_format, rotation, layout, width, lines,
            coefficients, alpha, planes)


def write_cases(path, cases):
    with open(path, "wb") as out:
        out.write(struct.pack("<I", len(cases)))
        for (input_format, output_format, rotation, layout, width, lines,
             coefficients, alpha, planes) in cases:
            out.write(struct.pack("<BBBBHH", input_format, output_format, rotation,
                                  layout, width, lines))
            out.write(struct.pack("<8h", *coefficients))
            out.write(struct.pack("<BBH", alpha, 0, 0))
            for plane in planes:
                out.write(struct.pack("<I", len(plane)))
                out.write(plane)


def read_results(path):
    results = []
    data = pathlib.Path(path).read_bytes()
    at = 0
    while at < len(data):
        (length,) = struct.unpack_from("<I", data, at)
        results.append(data[at + 4:at + 4 + length])
        at += 4 + length
    return results


def run_azahar(azahar, image, work, timeout):
    environment = dict(os.environ)
    environment["HEXLITH_ORACLE_DIR"] = str(work)
    environment["XDG_CONFIG_HOME"] = str(work / "config")
    environment["XDG_DATA_HOME"] = str(work / "data")
    (work / "done.txt").unlink(missing_ok=True)
    subprocess.run([azahar, image], env=environment, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not (work / "done.txt").exists():
        sys.exit("Azahar quit without running the cases")


def describe(case):
    input_format, output_format, rotation, layout, width, lines, *_ = case
    return (f"{INPUT_NAMES[input_format]} to {OUTPUT_NAMES[output_format]}, "
            f"rotation {rotation * 90}, {'tiles' if layout else 'lines'}, {width}x{lines}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cases", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=600)
    arguments = parser.parse_args()

    arguments.work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(arguments.seed)
    cases = [random_case(rng) for _ in range(arguments.cases)]
    write_cases(arguments.work / "y2r-cases.bin", cases)

    subprocess.run([arguments.tool, str(arguments.work)], check=True)
    run_azahar(arguments.azahar, arguments.image, arguments.work, arguments.timeout)

    hexlith = read_results(arguments.work / "y2r-results.hexlith.bin")
    azahar = read_results(arguments.work / "y2r-results.bin")
    if len(hexlith) != len(cases) or len(azahar) != len(cases):
        sys.exit(f"expected {len(cases)} results, Hexlith gave {len(hexlith)}, "
                 f"Azahar {len(azahar)}")

    disagreements = collections.Counter()
    shown = 0
    for case, ours, theirs in zip(cases, hexlith, azahar):
        if ours == theirs:
            continue
        disagreements[describe(case).split(",")[0]] += 1
        if shown < 5:
            first = next((i for i, (a, b) in enumerate(zip(ours, theirs)) if a != b),
                         min(len(ours), len(theirs)))
            print(f"differs: {describe(case)}, first at byte {first}, "
                  f"lengths {len(ours)} and {len(theirs)}")
            shown += 1
    agreed = len(cases) - sum(disagreements.values())
    print(f"{agreed} of {len(cases)} cases agree")
    for kind, count in sorted(disagreements.items()):
        print(f"  {count} differ: {kind}")
    return 0 if not disagreements else 1


if __name__ == "__main__":
    sys.exit(main())
