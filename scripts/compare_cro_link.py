#!/usr/bin/env python3
"""Compare Hexlith's linking of CRO modules with the memory of a running game.

The offline linker in src/domain/linker computes what every relocated word of a
module holds once the console's loader has placed and linked it. Its unit tests
check that it does what the code says. This checks that the code says what the
console does, by asking an emulator that runs the game for the same words.

Azahar logs, with Service.LDR at Debug, the three addresses each LoadCRO is given:
the mapping, the data buffer and the BSS buffer. From the log this script knows
which modules are resident and where. It writes a gdb command file that dumps
those modules out of Azahar through its GDB stub, then compares the dumps with
what hexlith-cro-link computes for the same modules at the same addresses.

Azahar's stub is slow, about 5 KB a second, and on 2026-10-04 it froze the
emulator twice during reads of a megabyte or more. So the dumps are written in
chunks of 64 KiB, each one a gdb command of its own, and the comparison uses
whatever chunks exist and says how much they covered.

What is compared, where a chunk covers it:

  - Every byte of each module's code and read-only data. A relocated word has to
    hold the value Hexlith computes, and every other byte has to be the file's.
    A byte that differs anywhere else is something the linker does not model.
  - Every relocated word in each module's data buffer. Other bytes of data are
    reported but are not a failure: the game writes to its own data.
  - Every word of the main executable that the static module's relocations
    patch.

Usage:
    scripts/compare_cro_link.py gdb LOG DUMP_DIR > dump.gdb
    gdb -batch -ex 'set remote memory-read-packet-size 512' \
        -ex 'set remote memory-read-packet-size fixed' \
        -ex 'target remote 127.0.0.1:24689' -x <some lines of dump.gdb>
    scripts/compare_cro_link.py compare DUMP_DIR --romfs ROMFS --tool hexlith-cro-link
    scripts/compare_cro_link.py --self-test
"""

import argparse
import pathlib
import re
import subprocess
import sys

CALLED = re.compile(r'LoadCRO:\d+: called \((?P<command>old|new)\), .*?'
                    r'cro_address=0x(?P<address>[0-9A-Fa-f]+), '
                    r'cro_size=0x(?P<size>[0-9A-Fa-f]+), '
                    r'data_segment_address=0x(?P<data>[0-9A-Fa-f]+), .*?'
                    r'data_segment_size=0x(?P<data_size>[0-9A-Fa-f]+), '
                    r'bss_segment_address=0x(?P<bss>[0-9A-Fa-f]+)')
LOADED = re.compile(r'CRO "(?P<name>[^"]*)" loaded at 0x(?P<base>[0-9A-Fa-f]+)'
                    r', fixed_end=0x(?P<end>[0-9A-Fa-f]+)')
UNLOADED = re.compile(r'Unloading CRO "(?P<name>[^"]*)"')

SEGMENT = re.compile(r'^segment (?P<name>\S+) index=(?P<index>\d+) type=(?P<type>\S+) '
                     r'file-offset=0x(?P<offset>[0-9a-f]+) size=0x(?P<size>[0-9a-f]+) '
                     r'address=0x(?P<address>[0-9a-f]+)')
WORD = re.compile(r'^word (?P<name>\S+) kind=(?P<kind>\S+) segment=(?P<segment>\d+) '
                  r'offset=0x(?P<offset>[0-9a-f]+) address=0x(?P<address>[0-9a-f]+) '
                  r'value=0x(?P<value>[0-9a-f]+)')

STATIC = "|static|"
RESIDENT_FILE = "resident.txt"

# The main executable's text segment, which the static module's relocations
# patch. Source: the extended header of the reference title, recorded in its
# GameProfile.
STATIC_TEXT_ADDRESS = 0x00100000
STATIC_TEXT_SIZE = 0x43BEDC


class Resident:
    """A module mapped at one moment of a run, with the addresses it was given."""

    def __init__(self, name, address, end, data, data_size, bss):
        self.name = name
        self.address = address
        self.end = end
        self.data = data
        self.data_size = data_size
        self.bss = bss

    def line(self):
        return (f"{self.name} address=0x{self.address:08x} end=0x{self.end:08x} "
                f"data=0x{self.data:08x} data-size=0x{self.data_size:x} bss=0x{self.bss:08x}")

    @staticmethod
    def parse(line):
        name, *fields = line.split()
        values = {key: int(value, 16) for key, value in (field.split("=") for field in fields)}
        return Resident(name, values["address"], values["end"], values["data"],
                        values["data-size"], values["bss"])


def resident_at_end(text):
    """The modules resident when the log ends, in the order they were loaded."""
    resident = []
    called = None
    for line in text.splitlines():
        match = CALLED.search(line)
        if match:
            called = match
            continue
        match = LOADED.search(line)
        if match:
            if called is None or int(called.group("address"), 16) != int(match.group("base"), 16):
                raise ValueError(f"a load of {match.group('name')} without its request line; "
                                 "is Service.LDR at Debug?")
            resident.append(Resident(match.group("name"), int(match.group("base"), 16),
                                     int(match.group("end"), 16), int(called.group("data"), 16),
                                     int(called.group("data_size"), 16),
                                     int(called.group("bss"), 16)))
            called = None
            continue
        match = UNLOADED.search(line)
        if match:
            resident = [module for module in resident if module.name != match.group("name")]
    return resident


# The size of one dump. Azahar's stub managed 64 KiB in fifteen seconds and
# froze on longer reads.
CHUNK = 0x10000


def chunk_commands(directory, start, end):
    lines = []
    for at in range(start, end, CHUNK):
        stop = min(at + CHUNK, end)
        lines.append(f"dump binary memory {directory}/{at:08x}.bin 0x{at:08x} 0x{stop:08x}")
    return lines


def gdb_commands(resident, directory):
    """The dumps in the order they are worth having: every module's code and data
    first, the main executable's text last because it is the largest."""
    lines = []
    for module in resident:
        lines += chunk_commands(directory, module.address, module.end)
        lines += chunk_commands(directory, module.data, module.data + module.data_size)
    lines += chunk_commands(directory, STATIC_TEXT_ADDRESS, STATIC_TEXT_ADDRESS + STATIC_TEXT_SIZE)
    return "\n".join(lines) + "\n"


class Memory:
    """Guest memory as far as the chunks in a directory cover it."""

    def __init__(self):
        self.chunks = []    # (address, bytes)

    def add(self, address, data):
        self.chunks.append((address, data))

    @staticmethod
    def from_directory(directory):
        memory = Memory()
        for path in sorted(directory.glob("*.bin")):
            try:
                address = int(path.stem, 16)
            except ValueError:
                continue
            memory.add(address, path.read_bytes())
        return memory

    def read(self, address, size):
        for start, data in self.chunks:
            if start <= address and address + size <= start + len(data):
                return data[address - start:address - start + size]
        return None


def read_link(text):
    """Segments and words from hexlith-cro-link, keyed by module name."""
    segments = {}
    words = {}
    for line in text.splitlines():
        match = SEGMENT.match(line)
        if match:
            segments.setdefault(match.group("name"), []).append({
                "index": int(match.group("index")), "type": match.group("type"),
                "offset": int(match.group("offset"), 16), "size": int(match.group("size"), 16),
                "address": int(match.group("address"), 16)})
            continue
        match = WORD.match(line)
        if match:
            words.setdefault(match.group("name"), []).append({
                "kind": match.group("kind"), "segment": int(match.group("segment")),
                "offset": int(match.group("offset"), 16),
                "address": int(match.group("address"), 16),
                "value": int(match.group("value"), 16)})
    return segments, words


class Comparison:
    def __init__(self):
        self.words_agreeing = 0
        self.words_disagreeing = []
        self.words_not_dumped = 0
        self.bytes_compared = 0
        self.unexplained_bytes = []
        self.data_bytes_changed = 0

    def word(self, memory, word, what):
        found = memory.read(word["address"], 4)
        if found is None:
            self.words_not_dumped += 1
            return
        found = int.from_bytes(found, "little")
        if found == word["value"]:
            self.words_agreeing += 1
        else:
            self.words_disagreeing.append(f"{what} at 0x{word['address']:08x}: Hexlith "
                                          f"0x{word['value']:08x}, memory 0x{found:08x}")

    def module(self, name, file, segments, words, memory):
        """One module: every relocated word, then every other byte of the file's
        segments, which in code and read-only data has to be the file's."""
        relocated = set()
        for word in words:
            self.word(memory, word, f"{name} {word['kind']} word")
            relocated.update(range(word["address"], word["address"] + 4))

        for segment in segments:
            if segment["size"] == 0 or segment["type"] in ("bss", "unknown"):
                continue
            for at in range(segment["size"]):
                address = segment["address"] + at
                if address in relocated:
                    continue
                found = memory.read(address, 1)
                if found is None:
                    continue
                expected = file[segment["offset"] + at]
                if segment["type"] == "data":
                    self.data_bytes_changed += found[0] != expected
                    continue
                self.bytes_compared += 1
                if found[0] != expected:
                    self.unexplained_bytes.append(f"{name} byte at 0x{address:08x}: file "
                                                  f"0x{expected:02x}, memory 0x{found[0]:02x}")

    def report(self):
        print(f"relocated words agreeing     {self.words_agreeing}")
        print(f"relocated words disagreeing  {len(self.words_disagreeing)}")
        print(f"relocated words not dumped   {self.words_not_dumped}")
        print(f"other code and rodata bytes  {self.bytes_compared} compared, "
              f"{len(self.unexplained_bytes)} differing from the file")
        print(f"data bytes the game changed  {self.data_bytes_changed}")
        for line in (self.words_disagreeing + self.unexplained_bytes)[:40]:
            print(f"  {line}")
        if self.words_disagreeing or self.unexplained_bytes:
            return 1
        print("Every relocated word that was dumped holds the value Hexlith computes, and every")
        print("other byte of code and read-only data that was dumped is the file's.")
        return 0


def compare(directory, romfs, tool):
    resident = [Resident.parse(line)
                for line in (directory / RESIDENT_FILE).read_text().splitlines() if line]
    arguments = [str(tool), str(romfs)]
    arguments += [f"{m.name}@{m.address:x},{m.data:x},{m.bss:x}" for m in resident]
    linked = subprocess.run(arguments, check=True, capture_output=True, text=True).stdout
    segments, words = read_link(linked)
    memory = Memory.from_directory(directory)

    comparison = Comparison()
    for module in resident:
        file = (romfs / f"{module.name}.cro").read_bytes()
        comparison.module(module.name, file, segments[module.name],
                          words.get(module.name, []), memory)
    for word in words.get(STATIC, []):
        comparison.word(memory, word, f"{STATIC} word")

    print(f"modules compared             {len(resident)}: "
          f"{', '.join(module.name for module in resident)}")
    print(f"bytes dumped                 {sum(len(data) for _, data in memory.chunks)}")
    return comparison.report()


def self_test():
    log = (
        "Service.LDR <Debug> ldr_ro.cpp:LoadCRO:191: called (old), cro_buffer_ptr=0x08044000, "
        "cro_address=0x006A5000, cro_size=0x4000, data_segment_address=0x08048000, zero=0, "
        "data_segment_size=0x10, bss_segment_address=0x08049000, bss_segment_size=0x20, "
        "auto_link=true, fix_level=1, crr_address=0x00000000\n"
        'CRO "DllA" loaded at 0x006A5000, fixed_end=0x006A9000\n'
        "Service.LDR <Debug> ldr_ro.cpp:LoadCRO:191: called (old), cro_buffer_ptr=0x08050000, "
        "cro_address=0x006A9000, cro_size=0x4000, data_segment_address=0x08054000, zero=0, "
        "data_segment_size=0x0, bss_segment_address=0x08055000, bss_segment_size=0x20, "
        "auto_link=true, fix_level=1, crr_address=0x00000000\n"
        'CRO "DllB" loaded at 0x006A9000, fixed_end=0x006AB000\n'
        'Unloading CRO "DllA"\n')
    resident = resident_at_end(log)
    assert [module.name for module in resident] == ["DllB"], resident
    assert resident[0].data == 0x08054000 and resident[0].bss == 0x08055000
    assert Resident.parse(resident[0].line()).line() == resident[0].line()
    commands = gdb_commands(resident, "out")
    assert "dump binary memory out/006a9000.bin 0x006a9000 0x006ab000" in commands, commands
    assert commands.count("\n") == 1 + (STATIC_TEXT_SIZE + CHUNK - 1) // CHUNK, \
        "an empty data segment is not dumped, and the text comes in chunks"

    try:
        resident_at_end('CRO "DllA" loaded at 0x006A5000, fixed_end=0x006A9000\n')
        raise AssertionError("a load without its request line was accepted")
    except ValueError:
        pass

    # A module of 0x20 bytes: code at file offset 0x10 for 8 bytes, data at 0x18
    # for 8, which the game was given a buffer for. One word in each is relocated.
    linked = (
        "segment DllB index=0 type=code file-offset=0x00000010 size=0x00000008 "
        "address=0x006a9010\n"
        "segment DllB index=1 type=data file-offset=0x00000018 size=0x00000008 "
        "address=0x08054000\n"
        "word DllB kind=internal segment=0 offset=0x00000004 address=0x006a9014 "
        "value=0x006a9010\n"
        "word DllB kind=external segment=1 offset=0x00000000 address=0x08054000 "
        "value=0x00100101 from=|static|\n")
    segments, words = read_link(linked)
    file = bytes(range(0x20))
    image = bytearray(file)
    image[0x14:0x18] = (0x006A9010).to_bytes(4, "little")
    data = bytearray(file[0x18:0x20])
    data[0:4] = (0x00100101).to_bytes(4, "little")
    data[6] = 0xEE

    def memory_of(image, data):
        memory = Memory()
        memory.add(0x006A9000, bytes(image))
        memory.add(0x08054000, bytes(data))
        return memory

    agreeing = Comparison()
    agreeing.module("DllB", file, segments["DllB"], words["DllB"], memory_of(image, data))
    assert agreeing.words_agreeing == 2, agreeing.words_agreeing
    assert not agreeing.words_disagreeing and not agreeing.unexplained_bytes
    assert agreeing.bytes_compared == 4
    assert agreeing.data_bytes_changed == 1

    image[0x11] ^= 0xFF
    image[0x14] ^= 0xFF
    disagreeing = Comparison()
    disagreeing.module("DllB", file, segments["DllB"], words["DllB"], memory_of(image, data))
    assert len(disagreeing.words_disagreeing) == 1
    assert len(disagreeing.unexplained_bytes) == 1

    # Nothing dumped: nothing compared, nothing failed.
    absent = Comparison()
    absent.module("DllB", file, segments["DllB"], words["DllB"], Memory())
    assert absent.words_not_dumped == 2 and absent.bytes_compared == 0

    print("self-test passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    commands = parser.add_subparsers(dest="command")

    gdb = commands.add_parser("gdb", help="write the gdb commands that dump the resident modules")
    gdb.add_argument("log", type=pathlib.Path)
    gdb.add_argument("directory", type=pathlib.Path)

    against = commands.add_parser("compare", help="compare the dumps with hexlith-cro-link")
    against.add_argument("directory", type=pathlib.Path)
    against.add_argument("--romfs", type=pathlib.Path, required=True)
    against.add_argument("--tool", type=pathlib.Path, required=True)

    options = parser.parse_args()
    if options.self_test:
        return self_test()

    if options.command == "gdb":
        resident = resident_at_end(options.log.read_text(errors="replace"))
        options.directory.mkdir(parents=True, exist_ok=True)
        (options.directory / RESIDENT_FILE).write_text(
            "".join(module.line() + "\n" for module in resident))
        sys.stdout.write(gdb_commands(resident, options.directory))
        return 0
    if options.command == "compare":
        return compare(options.directory, options.romfs, options.tool)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
