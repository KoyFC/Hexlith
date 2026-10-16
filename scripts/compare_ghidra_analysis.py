#!/usr/bin/env python3
"""Compare Hexlith's analysis tables with what Ghidra concludes about the same image.

hexlith-analyze walks the main executable from known entry points and writes
where functions begin and which bytes are code. Ghidra's auto-analysis answers
the same questions with different heuristics. Neither is the truth, and where
they disagree a third source settles it, the way Capstone and Unicorn settled
the decoder and the interpreter. What this script does is find the
disagreements and count them:

  - function entries both found, and those only one of them found, split by
    whether the other one has the address as code at all;
  - shared entries the two put in different instruction sets;
  - bytes of the text segment each calls code, and where the two overlap;
  - computed jumps Ghidra resolved to more than one target, which are jump
    tables, and how many of their targets Hexlith has not reached;
  - for each instruction set conflict hexlith-analyze reports, what Ghidra
    makes of that address.

Ghidra's side comes from scripts/ghidra/ExportAnalysisFacts.java, run headless
over the decompressed ExeFS .code, which writes addresses and counts only.
docs/classification.md has the command line and the results.

Usage:
    scripts/compare_ghidra_analysis.py --ghidra EXPORT_DIR --tables ANALYSIS_DIR \\
        --text-start 0x00100000 --text-size 0x43BEDC [--summary ANALYZE_OUTPUT]
    scripts/compare_ghidra_analysis.py --self-test
"""

import argparse
import bisect
import pathlib
import re
import sys
import tempfile

CONFLICT = re.compile(r'conflict at 0x(?P<address>[0-9A-Fa-f]+), reported (?P<count>\d+) time')


def read_records(path):
    """Yields (address, fields) for every record line of a table or an export."""
    for line in pathlib.Path(path).read_text().splitlines():
        if not line.startswith('0x'):
            continue
        parts = line.split()
        fields = dict(part.split('=', 1) for part in parts[1:] if '=' in part)
        yield int(parts[0], 16), fields, parts[1:]


class CodeMap:
    """Which instruction set, if any, each byte of the text segment is code in."""

    NOT_CODE = 0
    ARM = 1
    THUMB = 2

    def __init__(self, start, size):
        self.start = start
        self.size = size
        self.marks = bytearray(size)

    def mark(self, address, size, instruction_set):
        value = self.ARM if instruction_set == 'arm' else self.THUMB
        begin = max(address, self.start) - self.start
        end = min(address + size, self.start + self.size) - self.start
        if begin < end:
            self.marks[begin:end] = bytes([value]) * (end - begin)

    def at(self, address):
        if not self.start <= address < self.start + self.size:
            return self.NOT_CODE
        return self.marks[address - self.start]

    def code_bytes(self):
        return self.size - self.marks.count(self.NOT_CODE)


def set_name(mark):
    return {CodeMap.NOT_CODE: 'not code', CodeMap.ARM: 'arm', CodeMap.THUMB: 'thumb'}[mark]


def hexlith_code(tables, start, size):
    code = CodeMap(start, size)
    for address, fields, _ in read_records(tables / 'regions.txt'):
        if fields.get('kind') == 'code':
            code.mark(address, int(fields['size'], 16), fields['isa'])
    return code


def ghidra_code(export, start, size):
    code = CodeMap(start, size)
    for address, fields, _ in read_records(export / 'code.txt'):
        code.mark(address, int(fields['size'], 16), fields['isa'])
    return code


def function_entries(path, start, size):
    entries = {}
    for address, fields, _ in read_records(path):
        if start <= address < start + size:
            entries[address] = fields.get('isa')
    return entries


def jump_tables(export, start, size):
    """Computed jumps in the text with more than one resolved target."""
    tables = []
    for address, _, rest in read_records(export / 'jumps.txt'):
        targets = [int(word, 16) for word in rest if word.startswith('0x')]
        if start <= address < start + size and len(targets) > 1:
            tables.append((address, targets))
    return tables


def compare(export, tables, start, size, summary=None):
    """Returns the comparison as a list of (label, value) lines."""
    ours = hexlith_code(tables, start, size)
    theirs = ghidra_code(export, start, size)
    our_entries = function_entries(tables / 'functions.txt', start, size)
    their_entries = function_entries(export / 'functions.txt', start, size)

    both = set(our_entries) & set(their_entries)
    only_ours = set(our_entries) - set(their_entries)
    only_theirs = set(their_entries) - set(our_entries)
    lines = [
        ('functions, Hexlith', len(our_entries)),
        ('functions, Ghidra', len(their_entries)),
        ('entries both found', len(both)),
        ('entries in a different instruction set',
         sum(1 for address in both if our_entries[address] != their_entries[address])),
        ('entries only Hexlith found', len(only_ours)),
        ('  of them code to Ghidra', sum(1 for a in only_ours if theirs.at(a) != CodeMap.NOT_CODE)),
        ('entries only Ghidra found', len(only_theirs)),
        ('  of them code to Hexlith', sum(1 for a in only_theirs if ours.at(a) != CodeMap.NOT_CODE)),
    ]

    same = different = only_hexlith = only_ghidra = 0
    for our_mark, their_mark in zip(ours.marks, theirs.marks):
        if our_mark == their_mark:
            same += our_mark != CodeMap.NOT_CODE
        elif our_mark == CodeMap.NOT_CODE:
            only_ghidra += 1
        elif their_mark == CodeMap.NOT_CODE:
            only_hexlith += 1
        else:
            different += 1
    lines += [
        ('code bytes, Hexlith', ours.code_bytes()),
        ('code bytes, Ghidra', theirs.code_bytes()),
        ('code to both, same instruction set', same),
        ('code to both, different instruction set', different),
        ('code to Hexlith only', only_hexlith),
        ('code to Ghidra only', only_ghidra),
    ]

    tables_found = jump_tables(export, start, size)
    targets = sorted({target for _, list_of in tables_found for target in list_of})
    lines += [
        ('jump tables Ghidra resolved', len(tables_found)),
        ('  their jumps inside Hexlith code',
         sum(1 for address, _ in tables_found if ours.at(address) != CodeMap.NOT_CODE)),
        ('  distinct targets', len(targets)),
        ('  targets Hexlith has not reached',
         sum(1 for target in targets if ours.at(target) == CodeMap.NOT_CODE)),
    ]

    if summary is not None:
        for match in CONFLICT.finditer(pathlib.Path(summary).read_text()):
            address = int(match['address'], 16)
            function_starts = sorted(their_entries)
            index = bisect.bisect_right(function_starts, address) - 1
            entry = their_entries.get(address)
            note = set_name(theirs.at(address))
            if entry is not None:
                note += f', a function entry in {entry}'
            elif index >= 0:
                note += f', inside the function at 0x{function_starts[index]:08X}'
            lines.append((f'conflict 0x{address:08X} (x{match["count"]})', f'Ghidra: {note}'))
    return lines


def render(lines):
    width = max(len(label) for label, _ in lines)
    return ''.join(f'{label:<{width}}  {value}\n' for label, value in lines)


def self_test():
    start = 0x00100000
    size = 0x40
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        tables = root / 'tables'
        export = root / 'ghidra'
        tables.mkdir()
        export.mkdir()
        (tables / 'regions.txt').write_text(
            'format version=1\n'
            '0x00100000 size=0x00000010 kind=code isa=arm source=heuristic\n'
            '0x00100010 size=0x00000008 kind=data source=heuristic\n'
            '0x00100020 size=0x00000008 kind=code isa=thumb source=heuristic\n')
        (tables / 'functions.txt').write_text(
            '0x00100000 size=0x00000010 isa=arm source=export\n'
            '0x00100020 size=0x00000008 isa=thumb source=heuristic\n')
        (export / 'code.txt').write_text(
            '0x00100000 size=0x18 isa=arm\n'
            '0x00100020 size=0x4 isa=arm\n')
        (export / 'functions.txt').write_text(
            '0x00100000 isa=arm thunk=no\n'
            '0x00100020 isa=arm thunk=no\n'
            '0x00100008 isa=arm thunk=no\n'
            '0x00200000 isa=arm thunk=no\n')
        (export / 'jumps.txt').write_text(
            '0x00100004 targets=2 0x00100008 0x00100030\n'
            '0x0010000C targets=1 0x00100000\n')
        summary = root / 'summary.txt'
        summary.write_text('  conflict at 0x00100020, reported 2 time(s)\n'
                           '  conflict at 0x0010000A, reported 1 time(s)\n')

        result = dict(compare(export, tables, start, size, summary))
        expected = {
            'functions, Hexlith': 2,
            'functions, Ghidra': 3,  # the one outside the text is not counted
            'entries both found': 2,
            'entries in a different instruction set': 1,
            'entries only Ghidra found': 1,
            '  of them code to Hexlith': 1,
            'code bytes, Hexlith': 0x18,
            'code bytes, Ghidra': 0x1C,
            'code to both, same instruction set': 0x10,
            'code to both, different instruction set': 4,
            'code to Hexlith only': 4,
            'code to Ghidra only': 8,
            'jump tables Ghidra resolved': 1,
            '  their jumps inside Hexlith code': 1,
            '  distinct targets': 2,
            '  targets Hexlith has not reached': 1,
            'conflict 0x00100020 (x2)': 'Ghidra: arm, a function entry in arm',
            'conflict 0x0010000A (x1)': 'Ghidra: arm, inside the function at 0x00100008',
        }
        for label, value in expected.items():
            if result.get(label) != value:
                sys.exit(f'self-test failed: {label!r} is {result.get(label)!r}, expected {value!r}')
    print('self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ghidra', type=pathlib.Path, help='directory ExportAnalysisFacts wrote')
    parser.add_argument('--tables', type=pathlib.Path, help='directory hexlith-analyze wrote')
    parser.add_argument('--text-start', type=lambda text: int(text, 0))
    parser.add_argument('--text-size', type=lambda text: int(text, 0))
    parser.add_argument('--summary', type=pathlib.Path,
                        help="hexlith-analyze's output, for its conflict lines")
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return
    if None in (args.ghidra, args.tables, args.text_start, args.text_size):
        parser.error('--ghidra, --tables, --text-start and --text-size are required')
    sys.stdout.write(render(compare(args.ghidra, args.tables, args.text_start, args.text_size,
                                    args.summary)))


if __name__ == '__main__':
    main()
