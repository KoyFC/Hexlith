#!/usr/bin/env python3
"""Compare Hexlith's texture decoding with Azahar's.

Fourteen texture formats, a tiled layout in Morton order, and ETC1 blocks
with a differential mode whose sums the format says never overflow: a texel
read from the wrong place or widened the wrong way is a picture subtly off
with nothing to say why. So every texel of random textures in every format,
and of every texture the title draws with, is decoded by both sides and
compared.

Azahar decodes through Pica::Texture::LookupTexture at its first vertical
blank, under the oracle hooks of third_party/azahar/oracle.patch; Hexlith
through domain::gpu::DecodeTexture with tools/hexlith-oracle-run.

Usage:
    scripts/compare_pica_textures.py --azahar <azahar> --image <cartridge> \\
        --tool <hexlith-oracle-run> --work <dir> --random 5000
    tools/hexlith-oracle-run --texture-cases-from <recording> <dir>
    scripts/compare_pica_textures.py ... --cases <dir>/texture-cases.bin
"""

import argparse
import collections
import pathlib
import random
import struct
import subprocess
import sys

from azahar_oracle import CASE_FILES, clear, run_azahar

NAMES = ["RGBA8", "RGB8", "RGB5A1", "RGB565", "RGBA4", "LA8", "HILO8", "L8", "A8", "LA4",
         "L4", "A4", "ETC1", "ETC1A4"]
# The bytes an 8x8 tile of each format takes. Source: GBATEK, Texture Formats.
TILE_BYTES = [256, 192, 128, 128, 128, 128, 128, 64, 64, 64, 32, 32, 32, 64]


def random_case(rng):
    form = rng.randrange(len(NAMES))
    width = 8 * rng.randint(1, 8)
    height = 8 * rng.randint(1, 8)
    data = bytes(rng.getrandbits(8) for _ in range(TILE_BYTES[form] * (width // 8) * (height // 8)))
    return form, width, height, data


def write_cases(path, cases):
    with open(path, "wb") as out:
        out.write(struct.pack("<I", len(cases)))
        for form, width, height, data in cases:
            out.write(struct.pack("<IIII", form, width, height, len(data)))
            out.write(data)


def read_cases(path):
    data = pathlib.Path(path).read_bytes()
    (count,) = struct.unpack_from("<I", data, 0)
    at = 4
    cases = []
    for _ in range(count):
        form, width, height, length = struct.unpack_from("<IIII", data, at)
        at += 16
        cases.append((form, width, height, None))
        at += length
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--random", type=int, default=0)
    parser.add_argument("--cases", type=pathlib.Path, help="a case file already written")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1800)
    arguments = parser.parse_args()

    arguments.work.mkdir(parents=True, exist_ok=True)
    clear(arguments.work, CASE_FILES + ("texture-results.bin", "texture-results.hexlith.bin"))
    if arguments.cases:
        (arguments.work / "texture-cases.bin").write_bytes(arguments.cases.read_bytes())
        cases = read_cases(arguments.cases)
    else:
        rng = random.Random(arguments.seed)
        cases = [random_case(rng) for _ in range(arguments.random)]
        write_cases(arguments.work / "texture-cases.bin", cases)

    subprocess.run([arguments.tool, str(arguments.work)], check=True)
    run_azahar(arguments.azahar, arguments.image, arguments.work, arguments.timeout)
    ours = (arguments.work / "texture-results.hexlith.bin").read_bytes()
    theirs = (arguments.work / "texture-results.bin").read_bytes()

    at = 0
    differing = collections.Counter()
    texels = 0
    shown = 0
    for number, (form, width, height, _) in enumerate(cases):
        size = 4 * width * height
        mine, azahar = ours[at:at + size], theirs[at:at + size]
        at += size
        texels += width * height
        if mine != azahar:
            differing[NAMES[form]] += 1
            if shown < 10:
                first = next(i for i in range(0, size, 4) if mine[i:i + 4] != azahar[i:i + 4]) // 4
                print(f"case {number}, {NAMES[form]} {width}x{height}: texel ({first % width}, "
                      f"{first // width}) is {mine[4 * first:4 * first + 4].hex()} against "
                      f"{azahar[4 * first:4 * first + 4].hex()}")
                shown += 1
    print(f"{len(cases) - sum(differing.values())} of {len(cases)} textures agree, {texels} texels")
    for name, count in sorted(differing.items()):
        print(f"  {count} {name} textures differ")
    clear(arguments.work, ("texture-cases.bin",))
    return 0 if not differing else 1


if __name__ == "__main__":
    sys.exit(main())
