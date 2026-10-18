#!/usr/bin/env python3
"""Compare Hexlith's PICA command processor with Azahar's.

A command list is decoded once and drawn from many times, so a register
written to the wrong place, a FIFO that steps on once too often or a jump
followed one command late shows up as a wrong picture with nothing to say
why. So the decoder is run against Azahar's command processor, the way the ARM
decoder is run against Capstone: over random lists built to reach the
registers that do more than store, and over every list of a recording of the
reference title.

Both sides read the same case file. A case starts from a known state and is a
run of steps; a step writes blocks of memory, which is where lists jumped to
live, and then runs one list. After each step both sides report what the list
asked for, each draw, immediate vertex, primitive restart and FINALIZE with a
hash of the registers at that moment, and a hash of each section of the state:
the registers, each shader unit's code, descriptors and uniforms, the fixed
attributes, the lighting, fog and procedural texture tables, and the last
immediate vertex. Azahar runs the case file at its first vertical blank under
the oracle hooks of third_party/azahar/oracle.patch; Hexlith with
tools/hexlith-oracle-run.

The random lists stay out of the corners docs/formats/pica-registers.md lists
where Azahar is known to read a list differently from GBATEK, and out of the
inconsistencies between Azahar's own two ways of handling a run of writes, so
that what this reports is a difference worth looking at.

Usage:
    scripts/compare_pica_commands.py --azahar <azahar> --image <cartridge> \\
        --tool <hexlith-oracle-run> --work <dir> --random 2000
    scripts/compare_pica_commands.py ... --recording <work-dir>/<title>/gpu
"""

import argparse
import pathlib
import random
import struct
import subprocess
import sys

from azahar_oracle import run_azahar

SECTIONS = [
    "registers",
    "vertex shader code", "vertex shader descriptors", "vertex shader float uniforms",
    "vertex shader bool uniforms", "vertex shader integer uniforms",
    "geometry shader code", "geometry shader descriptors", "geometry shader float uniforms",
    "geometry shader bool uniforms", "geometry shader integer uniforms",
    "fixed attributes", "lighting tables", "fog table", "procedural texture tables",
    "immediate vertex",
]
EVENT_NAMES = {1: "draw arrays", 2: "draw elements", 3: "immediate vertex",
               4: "primitive restart", 5: "finalize"}

# Physical addresses. VRAM holds the lists a random case jumps to; the
# oracle puts the list a step runs at 185C0000h, so nothing else may.
VRAM = 0x18000000
LIST_SCRATCH = 0x185C0000
FCRAM = 0x20000000

# Register IDs, from GBATEK's register list.
FINALIZE = 0x010
DRAW_ARRAYS, DRAW_ELEMENTS = 0x22E, 0x22F
FIXED_INDEX, FIXED_DATA = 0x232, 0x233
CMDBUF_SIZE0, CMDBUF_ADDR0, CMDBUF_JUMP0 = 0x238, 0x23A, 0x23C
VSH_NUM_ATTR, VSH_COM_MODE, GEOSTAGE = 0x242, 0x244, 0x229
PRIMITIVE_CONFIG, RESTART = 0x25E, 0x25F
LIGHTING_INDEX, LIGHTING_DATA = 0x1C5, 0x1C8
FOG_INDEX, FOG_DATA = 0x0E6, 0x0E8
PROCTEX_INDEX, PROCTEX_DATA = 0x0AF, 0x0B0
GSH, VSH = 0x280, 0x2B0

# Registers a random write of any register leaves alone: the jumps, which
# Azahar follows through a null pointer when the memory is not there; the
# fixed attribute index, which past 0Fh makes Azahar overrun its queue; the
# lighting table index, whose table number Azahar does not check against its
# 24 tables; and the uniform indices, which the uniform commands set
# themselves.
AVOIDED = (CMDBUF_JUMP0, CMDBUF_JUMP0 + 1, FIXED_INDEX, LIGHTING_INDEX, GSH + 0x10, VSH + 0x10)

# The data ports of the FIFOs, and the registers whose write does something.
# Azahar handles a command that writes one of these several times in a batch:
# it stores the last value first, and it stops a uniform upload at the first
# uniform whose index is out of range. Its own handling of the same words one
# by one does neither, and Hexlith handles them one by one. A random command
# writes these once, or as a run of consecutive registers, or, for a uniform
# port, a whole number of uniforms in either format.
PORTS = (list(range(0x0B0, 0x0B8)) + list(range(0x0E8, 0x0F0)) + list(range(0x1C8, 0x1D0))
         + list(range(0x233, 0x236)) + list(range(0x291, 0x299)) + list(range(0x29C, 0x2A4))
         + list(range(0x2A6, 0x2AE)) + list(range(0x2C1, 0x2C9)) + list(range(0x2CC, 0x2D4))
         + list(range(0x2D6, 0x2DE)))
TRIGGERS = (FINALIZE, DRAW_ARRAYS, DRAW_ELEMENTS, FIXED_INDEX, PRIMITIVE_CONFIG, RESTART,
            GSH, GSH + 1, GSH + 2, GSH + 3, GSH + 4, VSH, VSH + 1, VSH + 2, VSH + 3, VSH + 4,
            VSH + 0x0D)
UNIFORM_PORTS = list(range(0x291, 0x299)) + list(range(0x2C1, 0x2C9))

# A register past the 0300h Azahar keeps, which Hexlith stores and neither
# compares nor acts on.
INERT = 0x3FF


class List:
    """A command list being written, in GBATEK's layout."""

    def __init__(self):
        self.words = []

    def command(self, register, values, mask=0xF, consecutive=False):
        self.words.append(values[0])
        self.words.append(register | (mask << 16) | ((len(values) - 1) << 20)
                          | (1 << 31 if consecutive else 0))
        self.words.extend(values[1:])
        if len(self.words) % 2:
            self.words.append(0)

    def bytes(self):
        return struct.pack("<%dI" % len(self.words), *self.words)

    def close(self):
        """Ends the list with a command of two words, to a register neither
        side gives a meaning to: when a list ends on a command of an odd number
        of words, Azahar skips the padding and reads one command past the end."""
        self.command(INERT, [0])
        return self


def word(rng):
    """A random word that is now and then a float24 or a small number."""
    kind = rng.random()
    if kind < 0.2:
        return rng.randrange(16)
    if kind < 0.4:
        return rng.randrange(1 << 24)
    return rng.getrandbits(32)


def random_command(rng, target):
    """One command aimed at a register that does something, or any register."""
    mask = 0xF if rng.random() < 0.8 else rng.randrange(16)
    count = rng.choice([1, 1, 1, 2, 3, 4, 6, 8, 12])
    unit = rng.choice([VSH, GSH])
    choice = rng.randrange(18)
    if choice == 0:
        index = rng.randrange(0x80) | (rng.choice([0, 1 << 31]))
        target.command(unit + 0x10, [index])
    elif choice == 1:
        # A batch of uniform words starts from an index it cannot run past:
        # at most eight uniforms, from below 88.
        uniform_words = rng.choice([1, 1, 12, 24])
        if uniform_words > 1:
            target.command(unit + 0x10, [rng.randrange(88) | rng.choice([0, 1 << 31])])
        target.command(unit + 0x11 + rng.randrange(8), [word(rng) for _ in range(uniform_words)], mask)
    elif choice == 2:
        # Code offsets that keep a run inside the unit's memory, or start
        # past its end: Azahar's two paths agree on both.
        limit = 512 if unit == VSH else 4096
        start = rng.choice([rng.randrange(limit - 16), limit + rng.randrange(64)])
        target.command(unit + 0x1B, [start])
        target.command(unit + 0x1C + rng.randrange(8), [word(rng) for _ in range(rng.randrange(1, 16))], mask)
    elif choice == 3:
        start = rng.choice([rng.randrange(4096 - 16), 4096 + rng.randrange(64)])
        target.command(unit + 0x25, [start])
        target.command(unit + 0x26 + rng.randrange(8), [word(rng) for _ in range(rng.randrange(1, 16))], mask)
    elif choice == 4:
        consecutive = rng.random() < 0.5
        target.command(unit + rng.randrange(0, 5),
                       [word(rng) for _ in range(count if consecutive else 1)], mask, consecutive)
    elif choice == 5:
        target.command(unit + 0x0D, [word(rng)], mask)
    elif choice == 6:
        # Never past 0Fh: Azahar then leaves its queue full and overruns it.
        target.command(FIXED_INDEX, [rng.choice([rng.randrange(16), 0xF, 0xF])])
    elif choice == 7:
        target.command(FIXED_DATA + rng.randrange(3), [word(rng) for _ in range(count)], mask)
    elif choice == 8:
        target.command(VSH_NUM_ATTR, [rng.randrange(16)])
    elif choice == 9:
        target.command(rng.choice([VSH_COM_MODE, GEOSTAGE]), [rng.randrange(4)])
    elif choice == 10:
        target.command(LIGHTING_INDEX, [rng.randrange(256) | (rng.randrange(24) << 8)])
        target.command(LIGHTING_DATA + rng.randrange(8), [word(rng) for _ in range(count)])
    elif choice == 11:
        target.command(FOG_INDEX, [rng.randrange(1 << 16)])
        target.command(FOG_DATA + rng.randrange(8), [word(rng) for _ in range(count)])
    elif choice == 12:
        target.command(PROCTEX_INDEX, [rng.randrange(256) | (rng.randrange(8) << 8)])
        target.command(PROCTEX_DATA + rng.randrange(8), [word(rng) for _ in range(count)])
    elif choice == 13:
        target.command(rng.choice([DRAW_ARRAYS, DRAW_ELEMENTS]), [word(rng)])
    elif choice == 14:
        target.command(rng.choice([PRIMITIVE_CONFIG, RESTART]), [word(rng)], mask)
    elif choice == 15:
        # FINALIZE alone, so that a run never reaches the interrupt setup.
        target.command(FINALIZE, [rng.choice([0x12345678, 0x12000000, 0, word(rng)])])
    else:
        # Anything from 0040h to 03FFh, one at a time or a run, but never a
        # jump: a jump to memory that is not there makes Azahar read through
        # a null pointer, where Hexlith refuses the list.
        consecutive = rng.random() < 0.5
        while True:
            register = rng.randrange(0x40, 0x400)
            last = register + (count - 1 if consecutive else 0)
            if any(register <= avoided <= last for avoided in AVOIDED):
                continue
            if not consecutive and count > 1 and (register in PORTS or register in TRIGGERS):
                continue
            break
        target.command(register, [word(rng) for _ in range(count)], mask, consecutive)


def random_case(rng):
    """A case of one to three steps, each with lists to jump to."""
    steps = []
    for _ in range(rng.randint(1, 3)):
        blocks = []
        targets = []
        # Lists to jump to, last first, each of which may jump to a later one.
        for index in range(rng.randrange(3)):
            jumped = List()
            for _ in range(rng.randint(1, 12)):
                random_command(rng, jumped)
            if targets and rng.random() < 0.5:
                add_jump(jumped, rng.choice(targets))
            data = jumped.close().bytes()
            data += b"\0" * (-len(data) % 16)
            address = VRAM + 0x100000 * index + 16 * rng.randrange(0x1000)
            blocks.append((address, data))
            targets.append((address, len(data)))
        main = List()
        for _ in range(rng.randint(1, 40)):
            random_command(rng, main)
        if targets and rng.random() < 0.7:
            add_jump(main, rng.choice(targets))
            for _ in range(rng.randint(0, 3)):
                random_command(rng, main)
        steps.append((blocks, main.close().bytes()))
    return steps


def add_jump(target, destination):
    address, size = destination
    target.command(CMDBUF_SIZE0, [size >> 3])
    target.command(CMDBUF_ADDR0, [address >> 3])
    target.command(CMDBUF_JUMP0, [1])


def recording_case(root):
    """Every list of a recording, in order, each after the pages before it."""
    steps = []
    blocks = []
    for line in (root / "recording.txt").read_text().splitlines()[1:]:
        fields = line.split()
        if fields[1] == "page":
            virtual = int(fields[2], 16)
            if 0x1F000000 <= virtual < 0x1F600000:
                physical = virtual - 0x1F000000 + VRAM
            else:
                physical = virtual - (0x14000000 if virtual < 0x30000000 else 0x30000000) + FCRAM
            blocks.append((physical, (root / "pages" / (fields[3] + ".bin")).read_bytes()))
        elif fields[1] == "list":
            steps.append((blocks, (root / "lists" / fields[4]).read_bytes()))
            blocks = []
    return steps


def write_cases(path, cases):
    with open(path, "wb") as out:
        out.write(struct.pack("<I", len(cases)))
        for steps in cases:
            out.write(struct.pack("<I", len(steps)))
            for blocks, data in steps:
                out.write(struct.pack("<I", len(blocks)))
                for address, block in blocks:
                    out.write(struct.pack("<II", address, len(block)))
                    out.write(block)
                out.write(struct.pack("<I", len(data)))
                out.write(data)


def read_results(path, cases):
    data = pathlib.Path(path).read_bytes()
    at = 0
    results = []
    for steps in cases:
        case = []
        for _ in steps:
            (count,) = struct.unpack_from("<I", data, at)
            at += 4
            events = []
            for _ in range(count):
                events.append(struct.unpack_from("<BQQ", data, at))
                at += 17
            hashes = struct.unpack_from("<16Q", data, at)
            at += 16 * 8
            registers = struct.unpack_from("<768I", data, at)
            at += 768 * 4
            case.append((events, hashes, registers))
        results.append(case)
    return results


def compare(cases, ours, theirs):
    """Prints the first differences and answers how many cases differ."""
    differing = 0
    shown = 0
    for number, (steps, mine, azahar) in enumerate(zip(cases, ours, theirs)):
        for step, ((my_events, my_hashes, my_registers), (their_events, their_hashes, their_registers)) in enumerate(zip(mine, azahar)):
            problems = []
            if [e[0] for e in my_events] != [e[0] for e in their_events]:
                problems.append("events: Hexlith %s, Azahar %s" % (
                    [EVENT_NAMES[e[0]] for e in my_events][:6],
                    [EVENT_NAMES[e[0]] for e in their_events][:6]))
            else:
                for index, (a, b) in enumerate(zip(my_events, their_events)):
                    if a != b:
                        part = "registers" if a[1] != b[1] else "inputs"
                        problems.append(f"event {index}, {EVENT_NAMES[a[0]]}: {part} differ")
                        break
            for name, a, b in zip(SECTIONS, my_hashes, their_hashes):
                if a != b:
                    problems.append(name + " differ")
            registers = [f"{i:03X}: {a:08X} against {b:08X}"
                         for i, (a, b) in enumerate(zip(my_registers, their_registers)) if a != b]
            if registers:
                problems.append("first registers that differ, Hexlith against Azahar: "
                                + ", ".join(registers[:4]))
            if problems:
                differing += 1
                if shown < 10:
                    print(f"case {number}, step {step}: " + "; ".join(problems))
                    shown += 1
                break
    return differing


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--random", type=int, default=0, help="random cases to run")
    parser.add_argument("--recording", type=pathlib.Path, help="a GPU recording to run")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1800)
    arguments = parser.parse_args()

    rng = random.Random(arguments.seed)
    cases = [random_case(rng) for _ in range(arguments.random)]
    if arguments.recording:
        cases.append(recording_case(arguments.recording))
    arguments.work.mkdir(parents=True, exist_ok=True)
    for stale in ("y2r-cases.bin", "pica-results.bin", "pica-results.hexlith.bin"):
        (arguments.work / stale).unlink(missing_ok=True)
    write_cases(arguments.work / "pica-cases.bin", cases)

    subprocess.run([arguments.tool, str(arguments.work)], check=True)
    run_azahar(arguments.azahar, arguments.image, arguments.work, arguments.timeout)

    ours = read_results(arguments.work / "pica-results.hexlith.bin", cases)
    theirs = read_results(arguments.work / "pica-results.bin", cases)
    steps = sum(len(case) for case in cases)
    events = sum(len(step[0]) for case in ours for step in case)
    differing = compare(cases, ours, theirs)
    print(f"{len(cases) - differing} of {len(cases)} cases agree, "
          f"{steps} lists and {events} events")
    return 0 if differing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
