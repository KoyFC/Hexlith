#!/usr/bin/env python3
"""Compare Hexlith's reference renderer and transfer engines with Azahar's.

Both sides replay a GPU recording of the reference title from memory that is
all zero and a PICA state reset as the command list comparison resets it.
A step writes the pages the recording holds before it, then either runs a
command list, drawing every triangle into memory, or carries out a GX
command's memory fills, display transfer or texture copy. After each step
both report a hash of the whole of VRAM, where the title keeps its colour and
depth buffers and the framebuffers its screens show, and a hash of a window
of memory the step names, for a transfer that writes outside VRAM.

The title leaves much of the fragment pipeline alone: it lights fragments
with one light and no tables, and never fogs one or tests the stencil. So
random steps are run too, each a list that sets every register a draw
reads to random values, draws a few random triangles into a small colour and
depth buffer of random formats and random contents, and samples random
textures, with random lighting and fog tables. Random memory fills, display
transfers and texture copies cover the transfer engines' formats and flags
the title never sets.

Hexlith samples textures as Azahar's software renderer does, the nearest
texel of the base level (ADR 0019), so that the two can agree to the bit.
Where a step's VRAM differs, the script runs both again to save VRAM after
the first few such steps and says where and by how much they differ.

Azahar runs the case file at its first vertical blank under the oracle hooks
of third_party/azahar/oracle.patch, configured for its software renderer and
its shader interpreter; Hexlith with tools/hexlith-oracle-run.

Usage:
    scripts/compare_pica_rasterizer.py --azahar <azahar> --image <cartridge> \\
        --tool <hexlith-oracle-run> --work <dir> \\
        --recording <work-dir>/<title>/gpu [--last-frame <n>]
    scripts/compare_pica_rasterizer.py ... --random 2000 --random-transfers 2000
"""

import argparse
import pathlib
import random
import struct
import subprocess
import sys

from azahar_oracle import clear, run_azahar, CASE_FILES

VRAM = 0x18000000
VRAM_SIZE = 0x00600000
FCRAM = 0x20000000

# Azahar's renderer settings for the run: the software renderer, and the
# shader interpreter rather than its JIT, which is what the shader
# comparison checked Hexlith's interpreter against.
RENDERER_SETTINGS = """[Renderer]
graphics_api=0
graphics_api\\default=false
use_shader_jit=false
use_shader_jit\\default=false
"""


def physical(virtual):
    """The physical address behind a game's address in VRAM or a linear heap."""
    if 0x1F000000 <= virtual < 0x1F600000:
        return virtual - 0x1F000000 + VRAM
    if 0x14000000 <= virtual < 0x1C000000:
        return virtual - 0x14000000 + FCRAM
    if 0x30000000 <= virtual < 0x38000000:
        return virtual - 0x30000000 + FCRAM
    return 0


# The GX commands that change memory: memory fill, display transfer, texture
# copy.
GX_STEPS = (2, 3, 4)
# How much of memory past a transfer's output to hash when it is outside VRAM.
WINDOW = 0x100000


def window_of(words):
    """The window to hash after a GX command: a transfer's output outside VRAM."""
    if words[0] & 0xFF in (3, 4):
        output = physical(words[2])
        if output >= FCRAM:
            return output, WINDOW
    return 0, 0


def recording_steps(root, last_frame):
    """Every list and every GX command that changes memory, in order, each
    after the pages recorded before it."""
    steps = []
    blocks = []
    for line in (root / "recording.txt").read_text().splitlines()[1:]:
        fields = line.split()
        if last_frame is not None and int(fields[0]) > last_frame:
            break
        if fields[1] == "page":
            blocks.append((physical(int(fields[2], 16)), root / "pages" / (fields[3] + ".bin")))
        elif fields[1] == "list":
            steps.append((int(fields[0]), blocks, root / "lists" / fields[4], (0, 0)))
            blocks = []
        elif fields[1] == "gx" and int(fields[2], 16) & 0xFF in GX_STEPS:
            words = tuple(int(word, 16) for word in fields[2:10])
            steps.append((int(fields[0]), blocks, words, window_of(words)))
            blocks = []
    return steps


# Random steps ------------------------------------------------------------------

# Where a random step puts things, in VRAM: a 64x64 colour buffer, its depth
# buffer, three textures and the vertex array. The list itself goes at
# LIST_SCRATCH, where the oracle puts it.
SIDE = 64
COLOR_BUFFER = VRAM
DEPTH_BUFFER = VRAM + 0x40000
TEXTURES = VRAM + 0x100000
TEXTURE_STRIDE = 0x10000
VERTICES = VRAM + 0x200000
LIST_SCRATCH = 0x185C0000

COLOR_BYTES = (4, 3, 2, 2, 2)
DEPTH_BYTES = {0: 2, 2: 3, 3: 4}
# Bits per texel of each texture format, GBATEK's 3DS GPU Texture Formats.
TEXEL_BITS = (32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8)

# The combiner's sources Azahar implements, and its colour modifiers.
SOURCES = (0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xD, 0xE, 0xF)
COLOR_MODIFIERS = (0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x8, 0x9, 0xC, 0xD)

# Seven outputs, each mapped to its semantics; 1Fh is none.
OUTPUT_MAPS = (0x03020100, 0x07060504, 0x0B0A0908, 0x1F100D0C, 0x1F1F0F0E, 0x1F141312,
               0x1F1F1716)


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def narrow(value, mantissa, exponent):
    """A float32 truncated to a PICA float with these widths."""
    if value == 0:
        return 0
    bits = float_bits(value)
    sign = bits >> 31
    biased = ((bits >> 23) & 0xFF) - 127 + (1 << (exponent - 1)) - 1
    biased = max(1, min(biased, (1 << exponent) - 2))
    return (sign << (mantissa + exponent)) | (biased << mantissa) | ((bits & 0x7FFFFF) >> (23 - mantissa))


def f24(value):
    return narrow(value, 16, 7)


class List:
    """A command list being written, in GBATEK's layout."""

    def __init__(self):
        self.words = []

    def write(self, register, *values, consecutive=False):
        values = list(values)
        self.words += [values[0], register | (0xF << 16) | ((len(values) - 1) << 20)
                       | ((1 << 31) if consecutive else 0)] + values[1:]
        if len(self.words) % 2:
            self.words.append(0)

    def bytes(self):
        return struct.pack(f"<{len(self.words)}I", *self.words)


def random_color(rng):
    return rng.getrandbits(32)


def random_float(rng, low, high):
    return rng.uniform(low, high)


def shader_setup(out):
    """A vertex shader that copies seven inputs to seven outputs."""
    vs = 0x2B0
    mov = lambda destination, source: (0x13 << 26) | (destination << 21) | (source << 12)
    out.write(vs + 0x1B, 0)
    out.write(vs + 0x1C, *[mov(n, n) for n in range(7)], 0x22 << 26)
    out.write(vs + 0x25, 0)
    out.write(vs + 0x26, 0xF | (0x1B << 5) | (0x1B << 14) | (0x1B << 23))
    out.write(vs + 0x09, 6)
    out.write(vs + 0x0A, 0)
    out.write(vs + 0x0B, 0x6543210)
    out.write(vs + 0x0C, 0)
    out.write(vs + 0x0D, 0x7F)
    out.write(0x242, 6)
    out.write(0x229, 0)
    out.write(0x244, 0)
    out.write(0x04F, len(OUTPUT_MAPS))
    out.write(0x050, *OUTPUT_MAPS, consecutive=True)


def vertex_array(out, rng, count):
    """The array of seven float4 attributes, and its bytes."""
    out.write(0x200, VERTICES >> 3)
    out.write(0x201, 0x0FFFFFFF)
    out.write(0x202, 6 << 28)
    out.write(0x203, 0, 0x6543210, (112 << 16) | (7 << 28), consecutive=True)
    values = []
    for _ in range(count):
        w = random_float(rng, 0.3, 3.0) if rng.random() < 0.9 else random_float(rng, -0.5, 0.3)
        values += [random_float(rng, -1.5, 1.5) * w, random_float(rng, -1.5, 1.5) * w,
                   random_float(rng, -1.2, 0.2) * w, w]
        values += [random_float(rng, -1, 1) for _ in range(4)]
        values += [random_float(rng, -0.2, 1.2) for _ in range(4)]
        values += [random_float(rng, -2, 3), random_float(rng, -2, 3), random_float(rng, 0.2, 2), 0]
        values += [random_float(rng, -2, 3), random_float(rng, -2, 3), 0, 0]
        values += [random_float(rng, -2, 2) for _ in range(3)] + [0]
        values += [random_float(rng, -2, 3), random_float(rng, -2, 3), 0, 0]
    return struct.pack(f"<{len(values)}f", *values)


def rasterizer_setup(out, rng):
    out.write(0x040, rng.randrange(4))
    out.write(0x041, f24(SIDE / 2))
    out.write(0x043, f24(SIDE / 2))
    # The viewport covers the buffer exactly: Azahar does not check that a
    # pixel is inside the buffer, and reads and writes past it.
    out.write(0x068, 0)
    clip = rng.random() < 0.25
    out.write(0x047, 1 if clip else 0)
    out.write(0x048, *[f24(random_float(rng, -1, 1)) for _ in range(4)], consecutive=True)
    out.write(0x04D, f24(-1.0) if rng.random() < 0.7 else f24(random_float(rng, -2, 2)))
    out.write(0x04E, f24(random_float(rng, 0, 1)) if rng.random() < 0.5 else 0)
    out.write(0x065, rng.choice((0, 0, 1, 3)))
    out.write(0x066, rng.randrange(SIDE) | (rng.randrange(SIDE) << 16))
    out.write(0x067, rng.randrange(SIDE) | (rng.randrange(SIDE) << 16))
    out.write(0x06D, rng.randrange(2))


def texture_setup(out, rng, blocks):
    units = ((0x081, 0x082, 0x083, 0x084, 0x085, 0x08E), (0x091, 0x092, 0x093, 0x094, 0x095, 0x096),
             (0x099, 0x09A, 0x09B, 0x09C, 0x09D, 0x09E))
    out.write(0x080, rng.randrange(8) | (rng.randrange(2) << 13))
    for index, (border, dimensions, parameters, levels, address, format_register) in enumerate(units):
        width, height = rng.choice((8, 16, 32, 64)), rng.choice((8, 16, 32, 64))
        texel_format = rng.randrange(14)
        kind = rng.choice((0, 0, 3, 5)) if index == 0 else 0
        out.write(border, random_color(rng))
        out.write(dimensions, (width << 16) | height)
        out.write(parameters, (rng.randrange(8) << 12) | (rng.randrange(8) << 8) | (kind << 28))
        out.write(levels, 0)
        location = TEXTURES + index * TEXTURE_STRIDE
        out.write(address, (location >> 3) if rng.random() < 0.95 else 0)
        out.write(format_register, texel_format)
        blocks.append((location, rng.randbytes(width * height * TEXEL_BITS[texel_format] // 8)))


def combiner_setup(out, rng):
    for base in (0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8):
        sources = 0
        for at in range(3):
            sources |= rng.choice(SOURCES) << (4 * at) | rng.choice(SOURCES) << (16 + 4 * at)
        operands = 0
        for at in range(3):
            operands |= rng.choice(COLOR_MODIFIERS) << (4 * at) | rng.randrange(8) << (12 + 4 * at)
        out.write(base, sources, operands, rng.randrange(10) | (rng.randrange(10) << 16),
                  random_color(rng), rng.randrange(4) | (rng.randrange(4) << 16), consecutive=True)
    fog = rng.choice((0, 5))
    out.write(0x0E0, fog | (rng.randrange(256) << 8) | (rng.randrange(2) << 16))
    out.write(0x0E1, random_color(rng))
    out.write(0x0FD, random_color(rng))
    out.write(0x0E6, 0)
    out.write(0x0E8, *[rng.getrandbits(24) for _ in range(128)])


def lighting_setup(out, rng):
    out.write(0x1C6, 0 if rng.random() < 0.6 else 1)
    f16 = lambda: narrow(random_float(rng, -4, 4), 10, 5)
    f20 = lambda: narrow(random_float(rng, -2, 2), 12, 7)
    for light in range(8):
        base = 0x140 + 0x10 * light
        out.write(base, *[rng.getrandbits(30) & 0x3FF3FF3FF if rng.random() < 0.3 else
                          (rng.randrange(256) << 20) | (rng.randrange(256) << 10) | rng.randrange(256)
                          for _ in range(4)], consecutive=True)
        out.write(base + 4, f16() | (f16() << 16), f16(), rng.getrandbits(13) | (rng.getrandbits(13) << 16),
                  rng.getrandbits(13), consecutive=True)
        out.write(base + 9, rng.randrange(16), f20(), f20(), consecutive=True)
    out.write(0x1C0, (rng.randrange(256) << 20) | (rng.randrange(256) << 10) | rng.randrange(256))
    out.write(0x1C2, rng.randrange(8))
    out.write(0x1C3, rng.getrandbits(32))
    # The bits that disable the red and blue reflection tables are the other
    # way round in Azahar from GBATEK (ADR 0019), so they are kept equal.
    config1 = rng.getrandbits(32)
    config1 = (config1 & ~(1 << 22)) | (((config1 >> 20) & 1) << 22)
    out.write(0x1C4, config1)
    out.write(0x1D0, rng.getrandbits(32))
    out.write(0x1D1, rng.getrandbits(32) & 0x07777777 if rng.random() < 0.5 else
              sum(rng.randrange(6) << (4 * n) for n in range(7)))
    out.write(0x1D2, rng.getrandbits(32) & 0x07777777)
    out.write(0x1D9, sum(rng.randrange(8) << (4 * n) for n in range(8)))
    for table in range(24):
        out.write(0x1C5, table << 8)
        out.write(0x1C8, *[rng.getrandbits(24) for _ in range(256)])


def merger_setup(out, rng, blocks):
    out.write(0x100, rng.randrange(2) << 8)
    equation = lambda: rng.randrange(5)
    factor = lambda: rng.randrange(15)
    out.write(0x101, equation() | (equation() << 8) | (factor() << 16) | (factor() << 20)
              | (factor() << 24) | (factor() << 28))
    out.write(0x102, rng.randrange(16))
    out.write(0x103, random_color(rng))
    out.write(0x104, rng.randrange(2) | (rng.randrange(8) << 4) | (rng.randrange(256) << 8))
    out.write(0x105, rng.getrandbits(32) & 0xFFFFFF71)
    out.write(0x106, rng.getrandbits(32) & 0x777)
    out.write(0x107, rng.getrandbits(32) & 0x1F71)
    out.write(0x113, 0xF if rng.random() < 0.9 else 0)
    out.write(0x115, rng.choice((0, 3, 3, 3)))
    color_format = rng.randrange(5)
    depth_format = rng.choice((0, 2, 3))
    out.write(0x116, depth_format)
    out.write(0x117, color_format << 16)
    out.write(0x11C, DEPTH_BUFFER >> 3)
    out.write(0x11D, COLOR_BUFFER >> 3)
    out.write(0x11E, SIDE | ((SIDE - 1) << 12))
    blocks.append((COLOR_BUFFER, rng.randbytes(SIDE * SIDE * COLOR_BYTES[color_format])))
    blocks.append((DEPTH_BUFFER, rng.randbytes(SIDE * SIDE * DEPTH_BYTES[depth_format])))


def random_step(rng):
    """Blocks of memory and a list that sets every register a draw reads."""
    out = List()
    blocks = []
    shader_setup(out)
    rasterizer_setup(out, rng)
    texture_setup(out, rng, blocks)
    combiner_setup(out, rng)
    lighting_setup(out, rng)
    merger_setup(out, rng, blocks)
    count = rng.randrange(3, 10)
    blocks.append((VERTICES, vertex_array(out, rng, count)))
    out.write(0x25E, rng.randrange(3) << 8)
    out.write(0x25F, 1)
    out.write(0x228, count)
    out.write(0x22A, 0)
    out.write(0x22E, 1)
    return blocks, out.bytes(), (0, 0)


# Where a random fill or transfer reads and writes, as a game's addresses:
# VRAM above the rasterizer's buffers, or the linear heap for an output.
TRANSFER_INPUT = 0x1F300000
TRANSFER_OUTPUT = 0x1F400000
LINEAR_OUTPUT = 0x14100000
FILL_AREA = 0x1F500000


def random_fill(rng, blocks):
    """A memory fill of one or both units, of any width, over random bytes."""
    words = [0x02]
    controls = 0
    for unit in range(2):
        if unit == 1 and rng.random() < 0.5:
            words += [0, 0, 0]
            continue
        # Addresses are in 16-byte units in GBATEK and 8-byte units in
        # Azahar; these round the same either way.
        start = FILL_AREA + unit * 0x40000 + rng.randrange(0, 0x1000, 16)
        length = rng.randrange(16, 0x8000, 16) + rng.randrange(8)
        words += [start, rng.getrandbits(32), start + length]
        controls |= (1 | (rng.randrange(4) << 8)) << (16 * unit)
        blocks.append((physical(start), rng.randbytes(length + 8)))
    words.append(controls)
    return tuple(words)


def random_display_transfer(rng, blocks):
    """A display transfer of random formats, sizes and flags over random
    pixels, into VRAM or the linear heap."""
    output_width = rng.choice((8, 16, 32, 64, 128, 240))
    output_height = rng.choice((8, 16, 32, 64, 128))
    input_width = output_width + rng.choice((0, 0, 8, 16))
    input_height = output_height + rng.choice((0, 0, 8))
    input_format = rng.randrange(5)
    output_format = rng.randrange(5)
    flags = (rng.randrange(2) | (rng.randrange(2) << 1) | (rng.randrange(2) << 2)
             | ((rng.random() < 0.2) << 5) | (input_format << 8) | (output_format << 12)
             | (rng.choice((0, 0, 0, 1, 2, 3)) << 24))
    output = TRANSFER_OUTPUT if rng.random() < 0.8 else LINEAR_OUTPUT
    blocks.append((physical(TRANSFER_INPUT), rng.randbytes(input_width * input_height * 4)))
    blocks.append((physical(output), rng.randbytes(0x80000)))
    return (0x03, TRANSFER_INPUT, output, input_width | (input_height << 16),
            output_width | (output_height << 16), flags, 0, 0)


def random_texture_copy(rng, blocks):
    """A texture copy of random lines and gaps over random bytes."""
    size = rng.randrange(0, 0x10000, 16) + rng.choice((0, 0, 8))
    widths = [rng.randrange(0, 64) | (rng.randrange(0, 5) << 16) for _ in range(2)]
    flags = (1 << 3) | rng.getrandbits(3)
    blocks.append((physical(TRANSFER_INPUT), rng.randbytes(0x60000)))
    blocks.append((physical(TRANSFER_OUTPUT), rng.randbytes(0x60000)))
    return (0x04, TRANSFER_INPUT, TRANSFER_OUTPUT, size, widths[0], widths[1], flags, 0)


def random_transfer_step(rng):
    """Blocks of memory and a GX command that fills or transfers."""
    blocks = []
    words = rng.choice((random_fill, random_display_transfer, random_display_transfer,
                        random_texture_copy))(rng, blocks)
    return blocks, words, window_of(words)


def write_cases(path, steps, dumps):
    with open(path, "wb") as out:
        out.write(struct.pack("<I", len(dumps)))
        for step in dumps:
            out.write(struct.pack("<I", step))
        out.write(struct.pack("<I", len(steps)))
        for _, blocks, operation, window in steps:
            out.write(struct.pack("<I", len(blocks)))
            for address, page in blocks:
                data = page if isinstance(page, bytes) else page.read_bytes()
                out.write(struct.pack("<II", address, len(data)))
                out.write(data)
            if isinstance(operation, tuple):
                out.write(struct.pack("<I8I", operation[0] & 0xFF, *operation))
            else:
                data = operation if isinstance(operation, bytes) else operation.read_bytes()
                out.write(struct.pack("<II", 1, len(data)))
                out.write(data)
            out.write(struct.pack("<II", *window))


def read_hashes(path):
    """Each step's two hashes, VRAM's and the window's, as a pair."""
    data = pathlib.Path(path).read_bytes()
    hashes = struct.unpack(f"<{len(data) // 8}Q", data)
    return list(zip(hashes[0::2], hashes[1::2]))


def run_both(arguments, steps, dumps):
    work = arguments.work
    clear(work, CASE_FILES + ("raster-results.bin", "raster-results.hexlith.bin"))
    for old in work.glob("raster-vram-*.bin"):
        old.unlink()
    write_cases(work / "raster-cases.bin", steps, dumps)
    config = work / "config" / "azahar-emu"
    config.mkdir(parents=True, exist_ok=True)
    (config / "qt-config.ini").write_text(RENDERER_SETTINGS)
    subprocess.run([arguments.tool, str(work)], check=True)
    run_azahar(arguments.azahar, arguments.image, work, arguments.timeout)
    return read_hashes(work / "raster-results.hexlith.bin"), read_hashes(work / "raster-results.bin")


def describe(work, step):
    """Where VRAM differs after a step: the spans of differing bytes."""
    ours = (work / f"raster-vram-{step}.hexlith.bin").read_bytes()
    theirs = (work / f"raster-vram-{step}.bin").read_bytes()
    spans = []
    start = None
    differing = 0
    for offset in range(VRAM_SIZE):
        if ours[offset] != theirs[offset]:
            differing += 1
            if start is None:
                start = offset
            end = offset
        elif start is not None and offset - end > 4096:
            spans.append((start, end))
            start = None
    if start is not None:
        spans.append((start, end))
    text = ", ".join(f"{VRAM + a:08X}-{VRAM + b:08X}" for a, b in spans[:6])
    return f"{differing} bytes differ, in {text}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--recording", type=pathlib.Path, help="a GPU recording to run")
    parser.add_argument("--last-frame", type=int, help="the last frame of the recording to run")
    parser.add_argument("--random", type=int, default=0, help="random lists to run after it")
    parser.add_argument("--random-transfers", type=int, default=0,
                        help="random fills and transfers to run after those")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--dumps", type=int, default=3,
                        help="differing steps whose VRAM to save and describe")
    parser.add_argument("--timeout", type=int, default=3600)
    arguments = parser.parse_args()
    arguments.work.mkdir(parents=True, exist_ok=True)

    steps = recording_steps(arguments.recording, arguments.last_frame) if arguments.recording else []
    rng = random.Random(arguments.seed)
    steps += [("random", *random_step(rng)) for _ in range(arguments.random)]
    steps += [("random", *random_transfer_step(rng)) for _ in range(arguments.random_transfers)]
    if not steps:
        sys.exit("nothing to run: give a recording, random steps or both")
    ours, theirs = run_both(arguments, steps, [])
    if len(ours) != len(steps) or len(theirs) != len(steps):
        sys.exit(f"{len(steps)} steps, but Hexlith answered {len(ours)} and Azahar {len(theirs)}")
    differing = [index for index, (a, b) in enumerate(zip(ours, theirs)) if a != b]
    print(f"{len(steps) - len(differing)} of {len(steps)} steps leave memory the same")
    if differing and arguments.dumps > 0:
        wanted = differing[:arguments.dumps]
        run_both(arguments, steps[:wanted[-1] + 1], wanted)
        for step in wanted:
            window = "" if ours[step][1] == theirs[step][1] else ", and the window differs"
            print(f"step {step} ({steps[step][0]}): {describe(arguments.work, step)}{window}")
    return 0 if not differing else 1


if __name__ == "__main__":
    sys.exit(main())
