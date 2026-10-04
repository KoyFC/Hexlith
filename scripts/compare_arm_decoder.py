#!/usr/bin/env python3
"""Compare Hexlith's ARM decoder with Capstone.

Unit tests over hand-written vectors cover the encodings someone thought to
write down. They cannot cover four billion words, and the mistakes that matter
in a decoder are the ones nobody thought of: a table row placed one position
off, a field read from the wrong end, a group refused outright. So the decoder
is checked against an independent implementation over the real image and over
millions of random words, and this is the script that does it.

Capstone is a development dependency and nothing else. The build does not use
it, the tests do not need it, and the decoder does not depend on it; it is a
second opinion, consulted on purpose and recorded in docs/arm-decoder.md.

Two differences are expected and are counted rather than treated as
disagreements. Both are documented in that note.

Hexlith refuses a word that breaks a field the architecture fixes, which
Capstone decodes anyway because it implements a later architecture where some of
those fields mean something. Hexlith reports a floating point instruction as
what it is encoded as, a coprocessor instruction on coprocessor 10 or 11, and
does not decode the operation itself.

Usage:
    scripts/compare_arm_decoder.py --image ~/recomp-data/work/.../exefs/.code
    scripts/compare_arm_decoder.py --random 1000000
"""

import argparse
import collections
import pathlib
import random
import subprocess
import sys

try:
    from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
    from capstone import arm as capstone_arm
except ImportError:
    sys.exit("capstone is not installed: dnf install python3-capstone")

DEFAULT_DUMP = pathlib.Path("build/ci/tools/hexlith-arm-dump/hexlith-arm-dump")

CONDITION_SUFFIX = {
    capstone_arm.ARM_CC_EQ: "eq",
    capstone_arm.ARM_CC_NE: "ne",
    capstone_arm.ARM_CC_HS: "hs",
    capstone_arm.ARM_CC_LO: "lo",
    capstone_arm.ARM_CC_MI: "mi",
    capstone_arm.ARM_CC_PL: "pl",
    capstone_arm.ARM_CC_VS: "vs",
    capstone_arm.ARM_CC_VC: "vc",
    capstone_arm.ARM_CC_HI: "hi",
    capstone_arm.ARM_CC_LS: "ls",
    capstone_arm.ARM_CC_GE: "ge",
    capstone_arm.ARM_CC_LT: "lt",
    capstone_arm.ARM_CC_GT: "gt",
    capstone_arm.ARM_CC_LE: "le",
}

# Where the two spell the same operation differently. An assembler prefers a
# name for a shape of an instruction; the encoding has one instruction.
ALIAS = {
    "push": "stm", "pop": "ldm",
    "stmda": "stm", "stmdb": "stm", "stmib": "stm", "stmia": "stm",
    "ldmda": "ldm", "ldmdb": "ldm", "ldmib": "ldm", "ldmia": "ldm",
    "srsda": "srs", "srsdb": "srs", "srsia": "srs", "srsib": "srs",
    "rfeda": "rfe", "rfedb": "rfe", "rfeia": "rfe", "rfeib": "rfe",
    "ldc2": "ldc", "stc2": "stc", "mrc2": "mrc", "mcr2": "mcr", "cdp2": "cdp",
    "mcrr2": "mcrr", "mrrc2": "mrrc", "ldc2l": "ldc", "stc2l": "stc",
    "ldcl": "ldc", "stcl": "stc", "pldw": "pld",
    "cpsie": "cps", "cpsid": "cps",
    # A branch into Java state behaves as a plain exchange on an ARM11, whose
    # Jazelle implementation cannot enter it.
    "bxj": "bx",
    # Taking the address of something is an addition to the program counter.
    "adr": "add",
    # A shift is a move whose operand is shifted.
    "lsl": "mov", "lsr": "mov", "asr": "mov", "ror": "mov", "rrx": "mov",
    # A halfword multiply's mnemonic is completed by which halves it reads.
    "smlabb": "smla", "smlabt": "smla", "smlatb": "smla", "smlatt": "smla",
    "smlalbb": "smlal", "smlalbt": "smlal", "smlaltb": "smlal", "smlaltt": "smlal",
    "smulbb": "smul", "smulbt": "smul", "smultb": "smul", "smultt": "smul",
    "smlawb": "smlaw", "smlawt": "smlaw", "smulwb": "smulw", "smulwt": "smulw",
    "smladx": "smlad", "smlsdx": "smlsd", "smuadx": "smuad", "smusdx": "smusd",
    "smlaldx": "smlald", "smlsldx": "smlsld",
    "smmlar": "smmla", "smmlsr": "smmls", "smmulr": "smmul",
}

# The thirty-six instructions Hexlith reports as one operation, with the lanes
# in a field of their own.
PARALLEL = {
    prefix + shape
    for prefix in ("s", "q", "sh", "u", "uq", "uh")
    for shape in ("add16", "asx", "sax", "sub16", "add8", "sub8")
}

# Capstone implements ARMv7 and ARMv8. These are not in ARMv6K, so answering
# Undefined for them is right.
NOT_IN_V6K = {
    "movw", "movt", "bfi", "bfc", "sbfx", "ubfx", "dmb", "dsb", "isb", "mls",
    "rbit", "dbg", "sdiv", "udiv", "pli", "hvc", "eret", "lda", "stl", "ldab",
    "stlb", "ldah", "stlh", "ldaex", "stlex", "ldaexb", "stlexb", "ldaexh",
    "stlexh", "ldaexd", "stlexd", "crc32b", "crc32h", "crc32w", "crc32cb",
    "crc32ch", "crc32cw", "trap", "hlt", "sevl", "udf", "hint",
    "ldrht", "ldrsbt", "ldrsht", "strht",
    "qasx", "qsax", "shasx", "shsax", "sasx", "ssax", "uasx", "usax",
    "uqasx", "uqsax", "uhasx", "uhsax",
    # Thumb-2, which ARMv6T2 added and the ARM11 does not have.
    "cbz", "cbnz", "tbb", "tbh", "orn", "pkh",
    # The security extension of ARMv8-M, which Capstone decodes in Thumb mode.
    "bxns", "blxns", "sg", "tt", "tta", "ttat", "ttt",
}


def is_if_then(name):
    """An IT block, which ARMv6T2 added. Its mnemonic spells out the block."""
    return name is not None and name.startswith("it") and set(name) <= {"i", "t", "e"}

# Where Hexlith is stricter than Capstone, because ARMv6K fixes a field the word
# does not respect and calls the result unpredictable.
STRICTER = {
    "cmp", "cmn", "tst", "teq", "mul", "smul", "smulw", "ldrd", "strd", "rfe",
    "srs", "uxtab", "uxtah", "sxtab", "sxtah", "uxtb", "uxth", "sxtb", "sxth",
    "uxtab16", "sxtab16", "uxtb16", "sxtb16", "mrs", "msr", "bx", "blx", "clz",
    "swp", "swpb", "qadd", "qsub", "qdadd", "qdsub", "sel", "pld", "setend",
    "cps", "mvn", "mov", "ssat16", "usat16", "rev", "rev16", "revsh", "ldrex",
    "strex", "ldrexb", "strexb", "ldrexh", "strexh", "ldrexd", "strexd",
    "smlald", "smlsld", "smmul", "smmla", "smmls", "usad8", "usada8", "smc",
    "pkhbt", "pkhtb", "ssat", "usat", "nop", "yield", "wfe", "wfi", "sev",
}

COPROCESSOR = {"ldc", "stc", "cdp", "mcr", "mrc", "mcrr", "mrrc"}
FLOAT_COPROCESSORS = (9, 10, 11)

TRANSFERS = {
    "ldr", "ldrb", "str", "strb", "ldrt", "ldrbt", "strt", "strbt",
    "ldrh", "strh", "ldrsb", "ldrsh", "ldrd", "strd",
}


def base_name(insn):
    """Capstone's mnemonic without the condition, the flag suffix or the width."""
    name = insn.mnemonic
    for part in (".w", ".n", ".f16", ".f32", ".f64", ".8", ".16", ".32", ".64"):
        name = name.replace(part, "")
    suffix = CONDITION_SUFFIX.get(insn.cc)
    if suffix and name.endswith(suffix) and len(name) > len(suffix):
        name = name[: -len(suffix)]
    if insn.update_flags and name.endswith("s") and len(name) > 1:
        name = name[:-1]
    return ALIAS.get(name, name)


def looks_floating_point(name):
    return name is None or (name.startswith(("v", "f")) and name not in ("sub", "sbc"))


def hexlith_name(register):
    """Capstone's register under the name Hexlith prints for it.

    An assembler calls r10, r11 and r12 sl, fp and ip. The encoding numbers them,
    and so does Hexlith, which names only the three that have a job of their own.
    """
    if register == capstone_arm.ARM_REG_SP:
        return "sp"
    if register == capstone_arm.ARM_REG_LR:
        return "lr"
    if register == capstone_arm.ARM_REG_PC:
        return "pc"
    if capstone_arm.ARM_REG_R0 <= register <= capstone_arm.ARM_REG_R12:
        return f"r{register - capstone_arm.ARM_REG_R0}"
    return None


def register_mask(insn, listed):
    mask = 0
    for register in listed:
        if register == capstone_arm.ARM_REG_SP:
            mask |= 1 << 13
        elif register == capstone_arm.ARM_REG_LR:
            mask |= 1 << 14
        elif register == capstone_arm.ARM_REG_PC:
            mask |= 1 << 15
        elif capstone_arm.ARM_REG_R0 <= register <= capstone_arm.ARM_REG_R12:
            mask |= 1 << (register - capstone_arm.ARM_REG_R0)
        else:
            return None
    return mask


class Comparison:
    def __init__(self, thumb=False):
        self.thumb = thumb
        self.engine = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
        self.engine.detail = True
        self.counts = collections.Counter()
        self.disagreements = collections.Counter()
        self.examples = {}

    def note(self, key, text):
        self.disagreements[key] += 1
        self.examples.setdefault(key, text)

    def check(self, record):
        fields = dict(
            part.split("=", 1) for part in record.split()[1:] if "=" in part
        )
        address = int(record.split()[0], 16)
        word = int(fields["encoding"], 16)
        mine = fields["op"]

        # A Thumb record is two bytes unless it is a branch with link, and the
        # encoding is printed at the width it was decoded at.
        width = 4 if len(fields["encoding"]) > 6 else 2
        encoded = word.to_bytes(width, "little")
        decoded = list(self.engine.disasm(encoded, address))
        insn = decoded[0] if decoded else None
        theirs = base_name(insn) if insn else None

        if mine == "parallel":
            agreed = theirs == fields["lanes"]
        elif mine == "undefined":
            if theirs is None or theirs in NOT_IN_V6K or theirs in PARALLEL or is_if_then(theirs):
                agreed = True
            elif looks_floating_point(theirs):
                agreed = True
            elif theirs in STRICTER:
                self.counts["stricter"] += 1
                return
            else:
                agreed = False
        elif mine in COPROCESSOR and (
            ((word >> 8) & 0xF) in FLOAT_COPROCESSORS or looks_floating_point(theirs)
        ):
            self.counts["floating point"] += 1
            return
        elif mine in ("ldr", "str") and theirs in ("ldm", "stm"):
            # A transfer of one register through the stack pointer is spelt push
            # or pop by an assembler and ldr or str by the encoding.
            agreed = True
        else:
            agreed = theirs == mine

        if not agreed:
            self.counts["disagreed"] += 1
            text = f"{insn.mnemonic} {insn.op_str}" if insn else "invalid"
            self.note(f"{mine} against {theirs}", f"0x{word:08X} hexlith={mine} capstone={text}")
            return

        self.counts["agreed"] += 1
        if insn is not None:
            self.check_fields(fields, insn, mine)

    def check_fields(self, fields, insn, mine):
        operands = insn.operands

        if "target" in fields:
            targets = [o.imm & 0xFFFFFFFF for o in operands if o.type == capstone_arm.ARM_OP_IMM]
            if targets and targets[0] != int(fields["target"], 16):
                self.note("branch target", f"{fields['encoding']} {insn.mnemonic} {insn.op_str}")
            self.counts["targets checked"] += 1

        if mine in TRANSFERS:
            memory = [o for o in operands if o.type == capstone_arm.ARM_OP_MEM]
            if memory:
                self.check_transfer(fields, insn, memory[0], operands)

        if mine in ("ldm", "stm") and "regs" in fields:
            listed = [o.reg for o in operands if o.type == capstone_arm.ARM_OP_REG]
            if insn.insn_name() not in ("push", "pop"):
                listed = listed[1:]
            mask = register_mask(insn, listed)
            if mask is not None:
                if mask != int(fields["regs"], 16):
                    self.note("register list", f"{fields['encoding']} {insn.mnemonic} {insn.op_str}")
                self.counts["register lists checked"] += 1

    def check_transfer(self, fields, insn, memory, operands):
        base = memory.mem.base
        if base != 0 and "rn" in fields:
            expected = hexlith_name(base)
            if expected is not None and fields["rn"] != expected:
                self.note("transfer base", f"{fields['encoding']} {insn.mnemonic} {insn.op_str}")
            self.counts["bases checked"] += 1

        if memory.mem.index == 0 and "imm" in fields:
            magnitude = int(fields["imm"], 16)
            mine = magnitude if fields.get("offset") == "add" else -magnitude
            if fields.get("index") == "pre":
                theirs = memory.mem.disp
            else:
                trailing = [o for o in operands if o.type == capstone_arm.ARM_OP_IMM]
                theirs = 0
                if trailing:
                    theirs = -trailing[0].imm if trailing[0].subtracted else trailing[0].imm
            if theirs != mine:
                self.note("transfer offset", f"{fields['encoding']} {insn.mnemonic} {insn.op_str}")
            self.counts["offsets checked"] += 1

        # Capstone marks write-back only for the pre-indexed spelling, so only
        # that one can be compared.
        if fields.get("index") == "pre":
            if insn.writeback != (fields.get("wb") == "yes"):
                self.note("transfer write-back", f"{fields['encoding']} {insn.mnemonic} {insn.op_str}")

    def report(self):
        total = self.counts["agreed"] + self.counts["disagreed"]
        print(f"words compared        {total}")
        print(f"agreed                {self.counts['agreed']}")
        print(f"disagreed             {self.counts['disagreed']}")
        print(f"hexlith stricter      {self.counts['stricter']}")
        print(f"floating point        {self.counts['floating point']}")
        for key in ("targets checked", "bases checked", "offsets checked",
                    "register lists checked"):
            print(f"{key:21} {self.counts[key]}")

        for key, count in self.disagreements.most_common(20):
            print(f"{count:8} {key:34} {self.examples[key]}")

        return 1 if self.disagreements else 0


def words_from_random(count, seed):
    generator = random.Random(seed)
    return "\n".join(f"0x{generator.getrandbits(32):08X}" for _ in range(count))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=pathlib.Path, default=DEFAULT_DUMP,
                        help="the hexlith-arm-dump binary to compare against")
    parser.add_argument("--image", type=pathlib.Path, help="a file of ARM code to sweep")
    parser.add_argument("--offset", default="0", help="where to start in that file")
    parser.add_argument("--length", default=None, help="how much of it to read")
    parser.add_argument("--address", default="0x00100000", help="the address the first word lives at")
    parser.add_argument("--thumb", action="store_true", help="decode as Thumb rather than ARM")
    parser.add_argument("--random", type=int, default=0, help="how many random words to sweep")
    parser.add_argument("--seed", type=int, default=1, help="the seed those words come from")
    options = parser.parse_args()

    if not options.dump.exists():
        sys.exit(f"{options.dump} is not built: cmake --build --preset ci")
    if not options.image and not options.random:
        sys.exit("name an image with --image or a count with --random")

    comparison = Comparison(options.thumb)
    prefix = [str(options.dump)] + (["--thumb"] if options.thumb else [])

    if options.image:
        arguments = prefix + [str(options.image), options.offset]
        if options.length:
            arguments += [options.length, options.address]
        output = subprocess.run(arguments, capture_output=True, text=True, check=True).stdout
        for line in output.splitlines():
            comparison.check(line)

    if options.random:
        output = subprocess.run(prefix + ["-"],
                                input=words_from_random(options.random, options.seed),
                                capture_output=True, text=True, check=True).stdout
        for line in output.splitlines():
            comparison.check(line)

    return comparison.report()


if __name__ == "__main__":
    sys.exit(main())
