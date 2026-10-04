#!/usr/bin/env python3
"""Compare Hexlith's ARM interpreter with Unicorn.

The interpreter is the semantic reference of ADR 0003, and a reference has to
be right in the corners nobody thought to write a test for: a carry out of a
shift by a register, which NaN a floating point addition returns, what a
saturating halving subtraction does to a negative lane. So it is run against an
independent implementation of the architecture, case by case, the way the
decoder is run against Capstone.

Unicorn is QEMU's processor emulation as a library, and it has an ARM11 MPCore
model with VFPv2. It is a development dependency and nothing else: the build
does not use it, the tests do not need it, and the interpreter does not depend
on it.

Each case is a random state, a random instruction, and one step. Both sides
start from the same registers, flags, FPSCR, floating point registers and
memory, and every register, the flags, FPSCR and every byte of memory are
compared afterwards.

What is not compared, and why, is counted rather than hidden:

- An instruction Hexlith stops on without executing, because it is undefined,
  unpredictable, privileged, unsupported, or would fault on alignment. Unicorn
  executes some of these anyway, since QEMU resolves unpredictable behaviour by
  choosing something, and models no alignment faults in this configuration.
- The cumulative exception flags of FPSCR, which ADR 0010 leaves unmodelled.
- A supervisor call or breakpoint, which Unicorn takes as an exception into a
  privileged mode the interpreter does not have.

A memory fault on one side must be a memory fault on the other.

Usage:
    python3 -m venv venv && venv/bin/pip install unicorn
    venv/bin/python scripts/compare_arm_interpreter.py --random 100000
    venv/bin/python scripts/compare_arm_interpreter.py --random 100000 --thumb
    venv/bin/python scripts/compare_arm_interpreter.py --random 100000 --float
    venv/bin/python scripts/compare_arm_interpreter.py --image <.code> --length 0x43BEDC
"""

import argparse
import collections
import pathlib
import random
import struct
import subprocess
import sys

try:
    from unicorn import (UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_INTR,
                         UC_ERR_FETCH_UNMAPPED, Uc, UcError)
    from unicorn import arm_const as reg
except ImportError:
    sys.exit("unicorn is not installed: pip install unicorn, in a virtual environment")

DEFAULT_RUN = pathlib.Path("build/ci/tools/hexlith-arm-run/hexlith-arm-run")
DEFAULT_DUMP = pathlib.Path("build/ci/tools/hexlith-arm-dump/hexlith-arm-dump")

CODE_BASE, CODE_SIZE = 0x00100000, 0x1000
DATA_BASE, DATA_SIZE = 0x00200000, 0x10000

# The CPSR bits user mode owns: N, Z, C, V, Q, GE and T.
CPSR_COMPARED = 0xF80F0020
USER_MODE = 0x10

# FPSCR without the cumulative exception flags, which ADR 0010 does not model.
FPSCR_COMPARED = ~0x9F & 0xFFFFFFFF

# FPEXC.EN, without which the floating point unit refuses every instruction.
FPEXC_ENABLE = 0x40000000

# The stops after which there is nothing to compare.
NOT_COMPARED = {"undefined-instruction", "unpredictable", "privileged", "unsupported",
                "alignment-fault", "supervisor-call", "breakpoint"}
FAULTS = {"read-fault", "write-fault", "fetch-fault"}


def pattern(address):
    """The byte both sides fill memory with, from hexlith-arm-run's usage text."""
    return ((address * 0x9E3779B1) & 0xFFFFFFFF) >> 24


def region_pattern(base, size):
    return bytes(pattern(base + offset) for offset in range(size))


CODE_PATTERN = region_pattern(CODE_BASE, CODE_SIZE)
DATA_PATTERN = region_pattern(DATA_BASE, DATA_SIZE)

# Floating point values worth more than a random pattern: zeros, infinities,
# NaNs of both kinds with payloads, denormals, the extremes, and ordinary
# numbers.
SPECIAL_SINGLES = [
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7F800000, 0xFF800000,
    0x7FC00000, 0xFFC00000, 0x7FC00123, 0x7F800001, 0xFF812345, 0x00000001, 0x80000001,
    0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F000000,
    0x3FC00000, 0x40200000, 0xC0600000, 0x34000000,
]


class Generator:
    def __init__(self, seed, thumb, float_space):
        self.random = random.Random(seed)
        self.thumb = thumb
        self.float_space = float_space

    def word(self):
        return self.random.getrandbits(32)

    def register(self):
        """A register value: often an address in the data region, sometimes a
        small number, sometimes anything."""
        choice = self.random.random()
        if choice < 0.45:
            offset = self.random.randrange(0x100, DATA_SIZE - 0x100)
            if self.random.random() < 0.7:
                offset &= ~3
            return DATA_BASE + offset
        if choice < 0.7:
            return self.random.randrange(0, 64)
        return self.word()

    def single(self):
        choice = self.random.random()
        if choice < 0.4:
            return self.random.choice(SPECIAL_SINGLES)
        if choice < 0.6:
            # A small integer, for the conversions.
            return self.random.randrange(0, 1 << 12) | (self.random.getrandbits(1) << 31)
        return self.word()

    def instruction(self):
        if self.thumb:
            return self.random.getrandbits(16) | (self.random.getrandbits(16) << 16)
        word = self.word()
        if self.float_space:
            word = (word & ~0x0C000E00) | 0x0C000A00
            # Bits 27 to 24 all set would make it a supervisor call.
            if (word >> 24) & 0xF == 0xF:
                word &= ~(1 << 24)
        # Most cases run unconditionally, so that the instruction executes.
        if self.random.random() < 0.75:
            word = (word & 0x0FFFFFFF) | 0xE0000000
        return word

    def case(self, instruction):
        registers = [self.register() for _ in range(15)]
        registers[13] = DATA_BASE + (self.random.randrange(0x400, DATA_SIZE - 0x400) & ~7)
        cpsr = self.random.getrandbits(5) << 27 | self.random.getrandbits(4) << 16
        if self.thumb:
            cpsr |= 1 << 5
        # Round to nearest, the mode the interpreter supports, with FZ and DN
        # either way and the flags anyhow.
        fpscr = self.random.getrandbits(4) << 28 | self.random.getrandbits(2) << 24
        singles = [self.single() for _ in range(32)]
        return {
            "registers": registers + [CODE_BASE],
            "cpsr": cpsr,
            "fpscr": fpscr,
            "singles": singles,
            "code": struct.pack("<I", instruction) + struct.pack("<I", 0),
        }


def case_line(case):
    fields = [f"r{n}=0x{value:X}" for n, value in enumerate(case["registers"])]
    fields.append(f"cpsr=0x{case['cpsr']:X}")
    fields.append(f"fpscr=0x{case['fpscr']:X}")
    fields += [f"s{n}=0x{value:X}" for n, value in enumerate(case["singles"])]
    fields.append("code=" + case["code"].hex())
    return " ".join(fields)


def parse_result(line):
    fields = dict(part.split("=", 1) for part in line.split())
    result = {
        "stop": fields["stop"],
        "registers": [int(fields[f"r{n}"], 16) for n in range(16)],
        "cpsr": int(fields["cpsr"], 16),
        "fpscr": int(fields["fpscr"], 16),
        "singles": [int(fields[f"s{n}"], 16) for n in range(32)],
        "memory": {},
    }
    if fields["mem"] != "-":
        for entry in fields["mem"].split(","):
            address, value = entry.split(":")
            result["memory"][int(address, 16)] = int(value, 16)
    return result


# How many cases one Unicorn instance runs before it is replaced.
UNICORN_LIFETIME = 5000

# r0 to r14 in Unicorn's numbering, which does not keep them contiguous.
GENERAL_REGISTERS = [getattr(reg, f"UC_ARM_REG_R{n}") for n in range(13)] + [
    reg.UC_ARM_REG_SP, reg.UC_ARM_REG_LR]


class Unicorn:
    """One Unicorn instance, reset before every case rather than rebuilt."""

    def __init__(self, thumb):
        self.thumb = thumb
        self.interrupted = False
        self.executed = 0
        self.create()

    def create(self):
        self.engine = Uc(UC_ARCH_ARM, UC_MODE_THUMB if self.thumb else UC_MODE_ARM)
        self.engine.ctl_set_cpu_model(reg.UC_CPU_ARM_11MPCORE)
        self.engine.mem_map(CODE_BASE, CODE_SIZE)
        self.engine.mem_map(DATA_BASE, DATA_SIZE)
        self.engine.hook_add(UC_HOOK_INTR, self.on_interrupt)
        self.engine.hook_add(UC_HOOK_CODE, self.on_instruction)

    def left_instruction_set(self):
        """Whether the last case exchanged into the other instruction set.

        Unicorn keeps executing in the instruction set a case ended in even when
        the next one writes the CPSR and starts at an address that says
        otherwise, and runs the next case's ARM code as Thumb. A fresh engine
        is the only reliable way back.
        """
        thumb_now = (self.engine.reg_read(reg.UC_ARM_REG_CPSR) >> 5) & 1
        return bool(thumb_now) != self.thumb

    def on_interrupt(self, engine, number, user_data):
        self.interrupted = True
        engine.emu_stop()

    def on_instruction(self, engine, address, size, user_data):
        # Called before each instruction. The second call means the first one
        # has finished, which is where the comparison wants to look. Unicorn's
        # own instruction count leaves the program counter of a branch behind.
        self.executed += 1
        if self.executed == 2:
            engine.emu_stop()

    def run(self, case):
        if self.left_instruction_set():
            self.create()
        engine = self.engine
        engine.mem_write(CODE_BASE, CODE_PATTERN)
        engine.mem_write(DATA_BASE, DATA_PATTERN)
        engine.mem_write(CODE_BASE, case["code"])

        # Unicorn keeps the code it has translated and does not notice that the
        # bytes under it changed, so every case after the first would run the
        # first case's instruction.
        engine.ctl_remove_cache(CODE_BASE, CODE_BASE + CODE_SIZE)

        # The mode goes first. The stack pointer and the link register are
        # banked, so writing them before the switch to user mode would set the
        # copies of whatever mode Unicorn starts in.
        engine.reg_write(reg.UC_ARM_REG_CPSR, case["cpsr"] | USER_MODE)
        engine.reg_write(reg.UC_ARM_REG_FPEXC, FPEXC_ENABLE)
        for register, value in zip(GENERAL_REGISTERS, case["registers"][:15]):
            engine.reg_write(register, value)
        for n, value in enumerate(case["singles"]):
            engine.reg_write(reg.UC_ARM_REG_S0 + n, value)
        engine.reg_write(reg.UC_ARM_REG_FPSCR, case["fpscr"])

        self.interrupted = False
        self.executed = 0
        start = CODE_BASE | (1 if self.thumb else 0)
        try:
            engine.emu_start(start, 0xFFFFFFFF)
        except UcError as error:
            # A branch to an address nothing is mapped at completes, and the
            # fetch after it is what fails.
            branched_away = error.errno == UC_ERR_FETCH_UNMAPPED and self.executed == 1
            if not branched_away:
                return {"error": str(error)}
        if self.interrupted:
            return {"error": "interrupt"}
        if self.executed > 2:
            raise RuntimeError("Unicorn ran past the instruction it was asked to stop after")

        memory = {}
        for base, expected in ((CODE_BASE, CODE_PATTERN), (DATA_BASE, DATA_PATTERN)):
            actual = bytes(engine.mem_read(base, len(expected)))
            if actual == expected:
                continue
            for offset, (now, before) in enumerate(zip(actual, expected)):
                if now != before:
                    memory[base + offset] = now
        return {
            "registers": [engine.reg_read(register) for register in GENERAL_REGISTERS]
            + [engine.reg_read(reg.UC_ARM_REG_PC)],
            "cpsr": engine.reg_read(reg.UC_ARM_REG_CPSR),
            "fpscr": engine.reg_read(reg.UC_ARM_REG_FPSCR),
            "singles": [engine.reg_read(reg.UC_ARM_REG_S0 + n) for n in range(32)],
            "memory": memory,
        }


class Comparison:
    def __init__(self):
        self.counts = collections.Counter()
        self.disagreements = collections.Counter()
        self.examples = {}

    def note(self, key, text):
        self.counts["disagreed"] += 1
        self.disagreements[key] += 1
        self.examples.setdefault(key, text)

    def check(self, case, mine, theirs, encoding):
        stop = mine["stop"]
        if stop in NOT_COMPARED:
            self.counts[f"hexlith stops: {stop}"] += 1
            return
        if stop in FAULTS:
            if "UNMAPPED" in theirs.get("error", ""):
                self.counts["both fault"] += 1
            else:
                self.note(f"only hexlith faults ({stop}, unicorn {theirs.get('error', 'ran')})",
                          f"0x{encoding:08X}")
            return
        if "error" in theirs:
            self.note(f"only unicorn stops ({theirs['error']})", f"0x{encoding:08X}")
            return

        differences = []
        for n in range(16):
            if mine["registers"][n] != theirs["registers"][n]:
                differences.append(f"r{n}")
        if (mine["cpsr"] ^ theirs["cpsr"]) & CPSR_COMPARED:
            differences.append("cpsr")
        if (mine["fpscr"] ^ theirs["fpscr"]) & FPSCR_COMPARED:
            differences.append("fpscr")
        for n in range(32):
            if mine["singles"][n] != theirs["singles"][n]:
                differences.append(f"s{n}")
        if mine["memory"] != theirs["memory"]:
            differences.append("memory")

        if differences:
            detail = ", ".join(
                f"{name} hexlith={self.value(mine, name)} unicorn={self.value(theirs, name)}"
                for name in differences[:3])
            self.note(f"0x{encoding & self.mask(encoding):08X} {differences[0]}",
                      f"0x{encoding:08X}: {detail}")
        else:
            self.counts["agreed"] += 1

    @staticmethod
    def mask(encoding):
        # Group disagreements by the opcode bits, so that one mistake does not
        # fill the report with every register combination that shows it.
        return 0x0FF000F0

    @staticmethod
    def value(result, name):
        if name.startswith("r"):
            return f"0x{result['registers'][int(name[1:])]:X}"
        if name.startswith("s"):
            return f"0x{result['singles'][int(name[1:])]:X}"
        if name in ("cpsr", "fpscr"):
            return f"0x{result[name]:X}"
        return str(sorted(result["memory"].items())[:4])

    def report(self):
        compared = self.counts["agreed"] + self.counts["disagreed"]
        print(f"cases compared        {compared}")
        print(f"agreed                {self.counts['agreed']}")
        print(f"disagreed             {self.counts['disagreed']}")
        print(f"both fault            {self.counts['both fault']}")
        for key in sorted(k for k in self.counts if k.startswith("hexlith stops")):
            print(f"{key:37} {self.counts[key]}")
        for key, count in self.disagreements.most_common(25):
            print(f"{count:8} {key:30} {self.examples[key]}")
        return 1 if self.disagreements else 0


def defined_only(dump, instructions, thumb):
    """The instructions Hexlith's decoder accepts, so that a run spends its
    cases on instructions rather than on words the interpreter refuses at once.
    The decoder is verified against Capstone separately, which is what makes it
    fair to let it choose."""
    arguments = [str(dump)] + (["--thumb"] if thumb else []) + ["-"]
    output = subprocess.run(arguments, input="\n".join(f"0x{word:08X}" for word in instructions),
                            capture_output=True, text=True, check=True).stdout
    return [word for word, line in zip(instructions, output.splitlines())
            if "op=undefined" not in line]


def words_from_image(path, offset, length, count, seed, thumb=False):
    """Words drawn from the image at random, at every word in ARM and every
    halfword in Thumb, where four bytes are read so that a branch with link
    brings its second half along."""
    data = pathlib.Path(path).read_bytes()[offset:offset + length]
    generator = random.Random(seed)
    step = 2 if thumb else 4
    positions = (len(data) - 4) // step
    return [struct.unpack_from("<I", data, step * generator.randrange(positions))[0]
            for _ in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run", type=pathlib.Path, default=DEFAULT_RUN,
                        help="the hexlith-arm-run binary to compare")
    parser.add_argument("--random", type=int, default=0, help="how many random instructions")
    parser.add_argument("--thumb", action="store_true", help="run Thumb rather than ARM")
    parser.add_argument("--float", action="store_true",
                        help="draw the instructions from the floating point space")
    parser.add_argument("--image", type=pathlib.Path,
                        help="draw the instructions from a file of ARM code instead")
    parser.add_argument("--offset", type=lambda text: int(text, 0), default=0)
    parser.add_argument("--length", type=lambda text: int(text, 0), default=None)
    parser.add_argument("--count", type=int, default=100000,
                        help="how many words to draw from the image")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--defined", action="store_true",
                        help="keep only the random words the decoder accepts")
    parser.add_argument("--dump", type=pathlib.Path, default=DEFAULT_DUMP,
                        help="the hexlith-arm-dump binary --defined filters with")
    options = parser.parse_args()

    if not options.run.exists():
        sys.exit(f"{options.run} is not built: cmake --build --preset ci")

    generator = Generator(options.seed, options.thumb, options.float)
    if options.image:
        length = options.length or options.image.stat().st_size - options.offset
        instructions = words_from_image(options.image, options.offset, length, options.count,
                                        options.seed, options.thumb)
    elif options.random:
        instructions = []
        while len(instructions) < options.random:
            drawn = [generator.instruction() for _ in range(options.random)]
            if options.defined:
                drawn = defined_only(options.dump, drawn, options.thumb)
            instructions += drawn
        instructions = instructions[:options.random]
    else:
        sys.exit("name an image with --image or a count with --random")

    cases = [generator.case(instruction) for instruction in instructions]
    output = subprocess.run([str(options.run)], input="\n".join(map(case_line, cases)) + "\n",
                            capture_output=True, text=True, check=True).stdout

    comparison = Comparison()
    for index, (case, line, instruction) in enumerate(zip(cases, output.splitlines(),
                                                          instructions)):
        # A Unicorn instance that has run some tens of thousands of cases stops
        # honouring emu_stop from a hook and runs on. A fresh one every few
        # thousand cases avoids it, and the check in run() would catch it.
        if index % UNICORN_LIFETIME == 0:
            unicorn = Unicorn(options.thumb)
        comparison.check(case, parse_result(line), unicorn.run(case), instruction)
    return comparison.report()


if __name__ == "__main__":
    sys.exit(main())
