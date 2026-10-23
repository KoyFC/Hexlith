#!/usr/bin/env python3
"""Compare the screens two backends drew from the same GPU recording.

hexlith-gpu-replay writes each screen of each frame asked for as
frame-<n>-top.png and frame-<n>-bottom.png. This compares two such
directories, normally the reference renderer's and the Vulkan backend's,
picture by picture, and holds each pair to the tolerance of ADR 0021:

- the mean difference over every channel of every pixel is at most 1.0;
- the 99.9th percentile of the differences is at most 12;
- at most 0.01% of the pixels differ by more than 12 in any channel.

The first two allow what Vulkan's fixed-function blending rounds where the
reference truncates, one step for each blended layer; the third allows the
few pixels where a triangle's coverage still differs. ADR 0021 has the
measurements behind each number.

Usage:
    scripts/compare_screens.py <reference-directory> <other-directory>
"""

import argparse
import pathlib
import struct
import sys
import zlib

MEAN_LIMIT = 1.0
PERCENTILE_LIMIT = 12
OUTLIER_DIFFERENCE = 12
OUTLIER_FRACTION = 0.0001


def load(path):
    """An eight-bit RGB PNG's width, height and rows, as hexlith-gpu-replay
    writes them: no interlace and the filter byte of every row zero."""
    data = pathlib.Path(path).read_bytes()
    at = 8
    width = height = 0
    compressed = b""
    while at < len(data):
        (length,) = struct.unpack(">I", data[at:at + 4])
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            compressed += body
        at += 12 + length
    raw = zlib.decompress(compressed)
    row = width * 3 + 1
    if any(raw[y * row] != 0 for y in range(height)):
        sys.exit(f"{path}: a row uses a PNG filter, which this script does not undo")
    return width, height, b"".join(raw[y * row + 1:(y + 1) * row] for y in range(height))


def measure(reference, other):
    """The mean difference, its 99.9th percentile, and the fraction of
    pixels with a channel beyond OUTLIER_DIFFERENCE."""
    width, height, first = load(reference)
    other_width, other_height, second = load(other)
    if (width, height) != (other_width, other_height):
        return None
    counts = [0] * 256
    outliers = 0
    for pixel in range(0, len(first), 3):
        worst = 0
        for channel in range(3):
            difference = abs(first[pixel + channel] - second[pixel + channel])
            counts[difference] += 1
            worst = max(worst, difference)
        if worst > OUTLIER_DIFFERENCE:
            outliers += 1
    total = len(first)
    mean = sum(value * count for value, count in enumerate(counts)) / total
    seen = 0
    percentile = 0
    for value, count in enumerate(counts):
        seen += count
        if seen >= total * 0.999:
            percentile = value
            break
    return mean, percentile, outliers / (width * height), max(v for v, c in enumerate(counts) if c)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("reference", type=pathlib.Path)
    parser.add_argument("other", type=pathlib.Path)
    arguments = parser.parse_args()

    failures = 0
    pictures = sorted(arguments.reference.glob("frame-*.png"),
                      key=lambda path: (int(path.name.split("-")[1]), path.name))
    if not pictures:
        sys.exit(f"no pictures in {arguments.reference}")
    for picture in pictures:
        other = arguments.other / picture.name
        if not other.exists():
            print(f"{picture.name}: missing from {arguments.other}")
            failures += 1
            continue
        measured = measure(picture, other)
        if measured is None:
            print(f"{picture.name}: the sizes differ")
            failures += 1
            continue
        mean, percentile, outliers, largest = measured
        within = mean <= MEAN_LIMIT and percentile <= PERCENTILE_LIMIT and outliers <= OUTLIER_FRACTION
        failures += 0 if within else 1
        print(f"{picture.name:24} mean {mean:5.3f}  99.9% {percentile:3}  beyond {OUTLIER_DIFFERENCE}: "
              f"{100 * outliers:6.3f}%  largest {largest:3}  {'within' if within else 'BEYOND'}")
    print(f"{len(pictures) - failures} of {len(pictures)} pictures within the tolerance of ADR 0021")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
