#!/usr/bin/env python3
"""Compare Hexlith's PICA shader interpreter with Azahar's.

The interpreter is the semantic reference of ADR 0018, and the GLSL the
Vulkan backend generates is checked against it, so it has to be right in the
corners: a product of zero and infinity, a NaN given to MAX in one place or
the other, a relative address that runs past the last uniform, four calls
ending on one instruction. So it is run against Azahar's interpreter the way
the ARM interpreter is run against Unicorn: a random state and one random
instruction at a time, then short random programs with control flow, then the
title's own programs with the uniforms it gave them.

Both sides read the same case file. Azahar runs it through
Pica::Shader::InterpreterEngine at its first vertical blank, under the oracle
hooks of third_party/azahar/oracle.patch; Hexlith through domain::gpu::RunShader
with tools/hexlith-oracle-run. Every register is compared bit for bit
afterwards, and for a geometry shader every primitive it emitted.

What is not compared, and why: CMP with the operators 6 and 7, which GBATEK
observed always hold and Azahar leaves unimplemented; a geometry shader's
EMIT of a fourth vertex, which Azahar stops on; and which NaN a NaN is. Any
NaN equals any other here: which operand's NaN a product or a sum passes on
depends on the order the compiler puts the operands in, and the hardware's
float24 has no payload to speak of.

With --against vulkan the other side is not Azahar but the Vulkan backend's
translation of each vertex shader program to GLSL (ADR 0020), run on the GPU
as a compute shader by hexlith-oracle-run --vulkan; geometry shaders stay on
the interpreter there, as in the backend. The translation is held to the
interpreter, which is held to Azahar.

Usage:
    scripts/compare_pica_shaders.py --azahar <azahar> --image <cartridge> \\
        --tool <hexlith-oracle-run> --work <dir> --single 100000 --programs 20000
    scripts/compare_pica_shaders.py --against vulkan --tool <hexlith-oracle-run> \\
        --work <dir> --single 20000 --programs 3000
"""

import argparse
import math
import pathlib
import random
import struct
import subprocess
import sys

from azahar_oracle import run_azahar

CODE_WORDS = 4096
UNIFORMS = 96
REGISTERS = 16

# Opcodes, from GBATEK's opcode summary.
ARITHMETIC = list(range(0x00, 0x20))
MOVA = 0x12
FLOW = {"BREAK": 0x20, "NOP": 0x21, "END": 0x22, "BREAKC": 0x23, "CALL": 0x24, "CALLC": 0x25,
        "CALLU": 0x26, "IFU": 0x27, "IFC": 0x28, "LOOP": 0x29, "EMIT": 0x2A, "SETEMIT": 0x2B,
        "JMPC": 0x2C, "JMPU": 0x2D}
END_WORD = FLOW["END"] << 26


def special_float(rng):
    """A float32 bit pattern: often a value that tests a rule."""
    kind = rng.random()
    if kind < 0.15:
        return rng.choice([0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000,
                           0x3F800000, 0xBF800000, 0x00000001, 0x80400000])
    if kind < 0.4:
        return struct.unpack("<I", struct.pack("<f", float(rng.randint(-300, 300)) / rng.choice([1, 2, 4, 8])))[0]
    if kind < 0.8:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-100.0, 100.0)))[0]
    return rng.getrandbits(32)


def vectors(rng, count):
    return [[special_float(rng) for _ in range(4)] for _ in range(count)]


def random_arithmetic(rng):
    """One instruction that does arithmetic, a compare or a multiply-add."""
    kind = rng.random()
    if kind < 0.15:
        # MAD or MADI.
        return (rng.choice([0x30, 0x38]) << 26) | rng.getrandbits(29)
    if kind < 0.25:
        # CMP, with operators 0 to 5 for both components. The opcode is five
        # bits, and bit 26 is the top bit of the x operator.
        return (0x17 << 27) | (rng.randrange(6) << 24) | (rng.randrange(6) << 21) | rng.getrandbits(21)
    return (rng.choice(ARITHMETIC) << 26) | rng.getrandbits(26)


def flow_word(opcode, target, count, extra=0):
    return (opcode << 26) | extra | ((target & 0xFFF) << 10) | (count & 0xFF)


def random_program(rng, geometry, length):
    """A program whose every jump goes forward, so that it ends."""
    code = []
    for pc in range(length):
        remaining = length - pc - 1
        kind = rng.random()
        if kind < 0.6 or remaining < 2:
            code.append(random_arithmetic(rng))
            continue
        target = pc + 1 + rng.randrange(remaining)
        count = rng.randrange(0, remaining - (target - pc - 1) + 1)
        condition = rng.getrandbits(4) << 22
        bool_id = rng.randrange(16) << 22
        name = rng.choice(["IFC", "IFU", "CALL", "CALLC", "CALLU", "JMPC", "JMPU", "LOOP",
                           "BREAK", "BREAKC", "NOP"] + (["EMIT", "SETEMIT"] * 2 if geometry else []))
        if name in ("IFC", "CALLC", "JMPC", "BREAKC"):
            code.append(flow_word(FLOW[name], target, count, condition))
        elif name in ("IFU", "CALLU", "JMPU"):
            code.append(flow_word(FLOW[name], target, count, bool_id))
        elif name == "CALL":
            code.append(flow_word(FLOW[name], target, count))
        elif name == "LOOP":
            code.append(flow_word(FLOW[name], target, 0, rng.randrange(4) << 22))
        elif name == "SETEMIT":
            code.append((FLOW[name] << 26) | (rng.randrange(3) << 24) | (rng.getrandbits(2) << 22))
        else:
            code.append(FLOW[name] << 26)
    code.append(END_WORD)
    return code


def random_case(rng, program=None, geometry=False, fill=None):
    """A random state and program. Code memory around the program is random
    words, or `fill`: the Vulkan translation covers every address a program
    could reach, and random words around it make that all of them."""
    entry = rng.randrange(CODE_WORDS - 64)
    code = [rng.getrandbits(32) if fill is None else fill for _ in range(CODE_WORDS)]
    body = program if program is not None else [random_arithmetic(rng), END_WORD]
    for offset, word in enumerate(body):
        code[entry + offset] = word
    # Relocate forward targets to the entry point.
    for offset, word in enumerate(body):
        opcode = word >> 26
        if 0x23 <= opcode <= 0x2D and opcode not in (0x2A, 0x2B):
            target = ((word >> 10) & 0xFFF) + entry
            code[entry + offset] = (word & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
    ints = [rng.randrange(5) | (rng.randrange(256) << 8) | (rng.randrange(-3, 4) & 0xFF) << 16
            | (rng.randrange(256) << 24) for _ in range(4)]
    return {
        "geometry": geometry,
        "entry": entry,
        "mask": rng.getrandbits(16),
        "code": code,
        "descriptors": [rng.getrandbits(32) for _ in range(CODE_WORDS)],
        "uniforms": vectors(rng, UNIFORMS),
        "bools": rng.getrandbits(16),
        "ints": ints,
        "inputs": vectors(rng, REGISTERS),
        "temporaries": vectors(rng, REGISTERS),
        "outputs": vectors(rng, 2 * REGISTERS),
        "bank": rng.getrandbits(1),
        "address": [rng.choice([rng.randrange(-140, 140), rng.getrandbits(32)]) & 0xFFFFFFFF
                    for _ in range(3)],
        "conditions": [rng.getrandbits(1), rng.getrandbits(1)],
    }


def put_vectors(out, vectors_):
    for vector in vectors_:
        out.write(struct.pack("<4I", *vector))


def write_cases(path, count, generate):
    """Writes cases as they are generated, keeping only a description of
    each: a quarter of a million cases would not fit in memory whole."""
    descriptions = []
    with open(path, "wb") as out:
        out.write(struct.pack("<I", count))
        for _ in range(count):
            case = generate()
            descriptions.append(describe(case))
            out.write(struct.pack("<III", int(case["geometry"]), case["entry"], case["mask"]))
            out.write(struct.pack("<%dI" % CODE_WORDS, *case["code"]))
            out.write(struct.pack("<%dI" % CODE_WORDS, *case["descriptors"]))
            put_vectors(out, case["uniforms"])
            out.write(struct.pack("<I", case["bools"]))
            out.write(struct.pack("<4I", *case["ints"]))
            for name in ("inputs", "temporaries", "outputs"):
                put_vectors(out, case[name])
            out.write(struct.pack("<I", case["bank"]))
            out.write(struct.pack("<3I", *case["address"]))
            out.write(struct.pack("<2I", *case["conditions"]))
    return descriptions


REGISTER_WORDS = 4 * REGISTERS * 4 + 1 + 3 + 2
# Cases a run of Azahar takes: each carries 32 KiB of code and descriptors.
BATCH = 20000
FLOAT_WORDS = 4 * REGISTERS * 4


def is_nan(word):
    return (word & 0x7F800000) == 0x7F800000 and (word & 0x007FFFFF) != 0


def same_float(a, b):
    return a == b or (is_nan(a) and is_nan(b))


def ordered(word):
    """A float's bits as an integer that counts up with its value."""
    return -(word & 0x7FFFFFFF) if word & 0x80000000 else word


def is_denormal_or_zero(word):
    return (word & 0x7F800000) == 0


# What Vulkan lets a device's arithmetic be off by: 2.5 ULP for a division,
# 2 for an inverse square root, 3 for exp2 and log2 (the Vulkan
# specification, Precision and Operation of SPIR-V Instructions), and a
# denormal flushed to zero by a device that does not promise to keep them.
VULKAN_ULPS = 3


def close_float(a, b):
    if same_float(a, b) or (is_denormal_or_zero(a) and is_denormal_or_zero(b)):
        return True
    return not is_nan(a) and not is_nan(b) and abs(ordered(a) - ordered(b)) <= VULKAN_ULPS


def same_primitive(mine, theirs):
    return mine[0] == theirs[0] and all(same_float(a, b) for a, b in zip(mine[1:], theirs[1:]))


def read_results(path, count):
    data = pathlib.Path(path).read_bytes()
    at = 0
    results = []
    for _ in range(count):
        registers = struct.unpack_from("<%dI" % REGISTER_WORDS, data, at)
        at += 4 * REGISTER_WORDS
        (primitives,) = struct.unpack_from("<I", data, at)
        at += 4
        emitted = []
        for _ in range(primitives):
            words = struct.unpack_from("<%dI" % (1 + 3 * REGISTERS * 4), data, at)
            at += 4 * len(words)
            emitted.append(words)
        results.append((registers, emitted))
    return results


def register_name(index):
    names = []
    for bank, count in (("v", REGISTERS), ("r", REGISTERS), ("o", REGISTERS), ("o'", REGISTERS)):
        for number in range(count):
            for component in "xyzw":
                names.append(f"{bank}{number}.{component}")
    names += ["bank", "a0.x", "a0.y", "aL", "cmp.x", "cmp.y"]
    return names[index]


def describe(case):
    words = case["code"][case["entry"]:case["entry"] + 3]
    return " ".join(f"{word:08X}" for word in words)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--against", choices=("azahar", "vulkan"), default="azahar")
    parser.add_argument("--azahar")
    parser.add_argument("--image")
    parser.add_argument("--device", default="", help="part of the Vulkan device's name")
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--single", type=int, default=0, help="single instructions")
    parser.add_argument("--programs", type=int, default=0, help="random programs")
    parser.add_argument("--cases", type=pathlib.Path,
                        help="a case file already written, such as the title's programs")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1800)
    arguments = parser.parse_args()
    if arguments.against == "azahar" and not (arguments.azahar and arguments.image):
        parser.error("comparing with Azahar needs --azahar and --image")
    other = "Azahar" if arguments.against == "azahar" else "Vulkan"

    rng = random.Random(arguments.seed)
    produced = [0]

    def generate():
        produced[0] += 1
        fill = END_WORD if arguments.against == "vulkan" else None
        if produced[0] <= arguments.single:
            return random_case(rng, fill=fill)
        geometry = rng.random() < 0.4
        return random_case(rng, random_program(rng, geometry, rng.randint(2, 40)), geometry, fill)

    arguments.work.mkdir(parents=True, exist_ok=True)
    for stale in ("y2r-cases.bin", "pica-cases.bin"):
        (arguments.work / stale).unlink(missing_ok=True)
    if arguments.cases:
        (count,) = struct.unpack("<I", arguments.cases.read_bytes()[:4])
        batches = [(count, None)]
    else:
        count = arguments.single + arguments.programs
        batches = [(min(BATCH, count - start), generate) for start in range(0, count, BATCH)]

    differing = 0
    close = 0
    number = 0
    for size, generator in batches:
        for stale in ("shader-results.bin", "shader-results.hexlith.bin", "shader-results.vulkan.bin"):
            (arguments.work / stale).unlink(missing_ok=True)
        if generator is None:
            (arguments.work / "shader-cases.bin").write_bytes(arguments.cases.read_bytes())
            descriptions = []
        else:
            descriptions = write_cases(arguments.work / "shader-cases.bin", size, generator)
        subprocess.run([arguments.tool, str(arguments.work)], check=True)
        if arguments.against == "azahar":
            run_azahar(arguments.azahar, arguments.image, arguments.work, arguments.timeout)
            theirs = read_results(arguments.work / "shader-results.bin", size)
        else:
            device = ["--device", arguments.device] if arguments.device else []
            subprocess.run([arguments.tool, "--vulkan", *device, str(arguments.work)], check=True)
            theirs = read_results(arguments.work / "shader-results.vulkan.bin", size)
        ours = read_results(arguments.work / "shader-results.hexlith.bin", size)
        for index, ((my_registers, my_primitives), (their_registers, their_primitives)) in enumerate(zip(ours, theirs)):
            problems = [f"{register_name(i)} {a:08X} against {b:08X}"
                        for i, (a, b) in enumerate(zip(my_registers, their_registers))
                        if not (same_float(a, b) if i < FLOAT_WORDS else a == b)]
            if problems and arguments.against == "vulkan" and all(
                    close_float(a, b) if i < FLOAT_WORDS else a == b
                    for i, (a, b) in enumerate(zip(my_registers, their_registers))):
                close += 1
                problems = []
            if len(my_primitives) != len(their_primitives):
                problems.append(f"{len(my_primitives)} primitives against {len(their_primitives)}")
            elif not all(same_primitive(a, b) for a, b in zip(my_primitives, their_primitives)):
                problems.append("primitives differ")
            if problems:
                differing += 1
                if differing <= 10:
                    where = descriptions[index] if index < len(descriptions) else ""
                    print(f"case {number + index} {where}: Hexlith against {other}: " + "; ".join(problems[:4]))
        number += size
    (arguments.work / "shader-cases.bin").unlink(missing_ok=True)
    if arguments.against == "vulkan":
        print(f"{count - differing - close} of {count} cases agree to the bit, "
              f"{close} within the precision Vulkan allows, {differing} beyond it")
    else:
        print(f"{count - differing} of {count} cases agree")
    return 0 if differing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
