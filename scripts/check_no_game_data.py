#!/usr/bin/env python3
"""Reject game-derived content before it can enter the repository.

Hexlith may contain facts about Pokemon X (hashes, addresses, symbol names)
but never bytes from it: no cartridge images, no extracted content, no CRO
modules, no disassembly, no lifted output. This script is the mechanical
enforcement of that rule. It runs from pre-commit on the staged files and
from CI across everything git tracks.

The checks are deliberately blunt. A false positive costs one line in the
allowlist; a false negative puts copyrighted code in a public repository.

Run the built-in tests with --self-test. They use nothing but the standard
library so the guard stays verifiable without extra packages installed.
"""

from __future__ import annotations

import argparse
import math
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

# A tracked file above this size is almost certainly not hand-written. The
# limit is low on purpose: it is a tripwire, not a storage policy.
MAX_TRACKED_FILE_BYTES = 256 * 1024

# Bytes read from the front of a file to test for container signatures. Large
# enough to cover the NCCH header at 0x100 and the CRO magic at 0x80.
HEADER_BYTES = 0x200

# Entropy is sampled rather than computed over the whole file, which keeps the
# check cheap on the largest files the size limit still allows.
ENTROPY_SAMPLE_BYTES = 64 * 1024
MIN_BYTES_FOR_ENTROPY_CHECK = 4 * 1024

# Compressed or encrypted data sits near the theoretical maximum of 8 bits per
# byte. Source code and documentation sit near 4.5. The threshold leaves a
# wide margin on both sides.
ENTROPY_LIMIT_BITS_PER_BYTE = 7.2

# How many matching lines it takes before a text file is called a dump. One
# stray line in prose is not evidence; eight consecutive-looking ones are.
MIN_SUSPICIOUS_LINES = 8

ALLOWLIST_PATH = Path("scripts/game-data-guard-allowlist.txt")

# Extensions that can only ever hold game content or console secrets.
DENIED_SUFFIXES = frozenset(
    {
        ".3ds",
        ".aac",
        ".app",
        ".bch",
        ".bcres",
        ".bin",
        ".cci",
        ".cdc",
        ".cfa",
        ".cia",
        ".code",
        ".cro",
        ".crr",
        ".crs",
        ".cxi",
        ".disasm",
        ".exefs",
        ".garc",
        ".key",
        ".keys",
        ".lst",
        ".moflex",
        ".ncch",
        ".ncsd",
        ".romfs",
        ".shbin",
        ".tik",
        ".tmd",
    }
)

# Container magic values, each at the offset where the format puts it.
# Sources: 3dbrew.org NCSD, NCCH, RomFS and CRO0 pages.
DENIED_SIGNATURES = (
    (0x000, b"IVFC", "RomFS IVFC container"),
    (0x000, b"SMDH", "title icon and metadata"),
    (0x000, b"CRR0", "CRR module hash container"),
    (0x080, b"CRO0", "CRO dynamic module"),
    (0x080, b"FIXD", "CRO dynamic module, already relocated"),
    (0x100, b"NCSD", "NCSD cartridge image"),
    (0x100, b"NCCH", "NCCH content container"),
)

# An objdump or Ghidra listing line: an address, a 32-bit encoding, and a
# mnemonic. Matches "00100000: e92d4ff0 push" and "0x100000 e92d4ff0 stmdb".
DISASSEMBLY_LINE = re.compile(
    r"^\s*(?:0x)?[0-9a-fA-F]{6,8}:?\s+[0-9a-fA-F]{8}\s+[a-z][a-z0-9.]{1,8}\b"
)

# A hexdump row: at least eight separated byte pairs. A documentation table
# row such as "| 0x100 | 4 | Magic |" does not match.
HEXDUMP_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]{2}[\s:]+){8,}")


class Violation:
    """One reason a single file must not be tracked."""

    def __init__(self, path: Path, reason: str) -> None:
        self.path = path
        self.reason = reason

    def __str__(self) -> str:
        return f"{self.path}: {self.reason}"


def load_allowlist(root: Path) -> set[str]:
    """Read the paths exempt from the size and content checks."""
    allowlist_file = root / ALLOWLIST_PATH
    if not allowlist_file.is_file():
        return set()
    entries = set()
    for raw_line in allowlist_file.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if line:
            entries.add(line)
    return entries


def shannon_entropy(data: bytes) -> float:
    """Return the entropy of data in bits per byte."""
    if not data:
        return 0.0
    counts = Counter(data)
    total = len(data)
    return -sum(
        (count / total) * math.log2(count / total) for count in counts.values()
    )


def count_matching_lines(text: str, pattern: re.Pattern[str]) -> int:
    return sum(1 for line in text.splitlines() if pattern.match(line))


def check_signatures(header: bytes) -> str | None:
    for offset, magic, description in DENIED_SIGNATURES:
        if header[offset : offset + len(magic)] == magic:
            return (
                f"starts with the {magic.decode('ascii')} magic at offset "
                f"{offset:#x}, so it is a {description}"
            )
    return None


def check_text_content(path: Path) -> str | None:
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return None

    disassembly_lines = count_matching_lines(text, DISASSEMBLY_LINE)
    if disassembly_lines >= MIN_SUSPICIOUS_LINES:
        return f"contains {disassembly_lines} lines that look like disassembly"

    hexdump_lines = count_matching_lines(text, HEXDUMP_LINE)
    if hexdump_lines >= MIN_SUSPICIOUS_LINES:
        return f"contains {hexdump_lines} lines that look like a hex dump"

    return None


def check_file(path: Path, allowlist: set[str]) -> list[Violation]:
    """Return every reason this file must not be tracked."""
    if not path.is_file():
        return []

    relative = path.as_posix()
    exempt = relative in allowlist
    violations = []

    if path.suffix.lower() in DENIED_SUFFIXES:
        violations.append(
            Violation(path, f"has the {path.suffix} extension, reserved for game data")
        )

    with path.open("rb") as handle:
        header = handle.read(HEADER_BYTES)
        sample = header + handle.read(ENTROPY_SAMPLE_BYTES - len(header))

    signature_reason = check_signatures(header)
    if signature_reason:
        violations.append(Violation(path, signature_reason))

    if b"\0" in header and not exempt:
        violations.append(Violation(path, "holds binary content, which is never tracked"))

    size = path.stat().st_size
    if size > MAX_TRACKED_FILE_BYTES and not exempt:
        violations.append(
            Violation(
                path,
                f"is {size} bytes, above the {MAX_TRACKED_FILE_BYTES} byte limit. "
                f"Add it to {ALLOWLIST_PATH} with a reason if it belongs here.",
            )
        )

    if len(sample) >= MIN_BYTES_FOR_ENTROPY_CHECK and not exempt:
        entropy = shannon_entropy(sample)
        if entropy > ENTROPY_LIMIT_BITS_PER_BYTE:
            violations.append(
                Violation(
                    path,
                    f"has an entropy of {entropy:.2f} bits per byte, so it is "
                    "compressed, encrypted or otherwise not hand-written",
                )
            )

    text_reason = check_text_content(path)
    if text_reason:
        violations.append(Violation(path, text_reason))

    return violations


def tracked_files(root: Path) -> list[Path]:
    output = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    ).stdout
    return [root / name for name in output.decode("utf-8").split("\0") if name]


def run_checks(paths: list[Path], root: Path) -> list[Violation]:
    allowlist = load_allowlist(root)
    violations = []
    for path in paths:
        violations.extend(check_file(path, allowlist))
    return violations


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, help="files to check")
    parser.add_argument(
        "--all", action="store_true", help="check every file git tracks"
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run the guard's own tests"
    )
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        return self_test()

    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    )

    paths = tracked_files(root) if arguments.all else arguments.paths
    violations = run_checks(paths, root)

    for violation in violations:
        print(f"error: {violation}", file=sys.stderr)

    if violations:
        print(
            f"\n{len(violations)} problem(s) found. Hexlith never stores anything "
            "derived from the game; extracted content belongs in the workspace "
            "directory outside this repository.",
            file=sys.stderr,
        )
        return 1
    return 0


def self_test() -> int:
    """Check the guard against crafted inputs. Prints a line per case."""
    failures = 0

    def expect(name: str, content: bytes, should_fail: bool, filename: str = "sample.txt"):
        nonlocal failures
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / filename
            path.write_bytes(content)
            violations = check_file(path, allowlist=set())
            failed = bool(violations)
            if failed != should_fail:
                verdict = "accepted" if not failed else "rejected"
                wanted = "rejected" if should_fail else "accepted"
                print(f"FAIL {name}: {verdict}, expected {wanted}")
                failures += 1
            else:
                print(f"ok   {name}")

    def padded(magic: bytes, offset: int) -> bytes:
        return b"\x01" * offset + magic + b"\x02" * (HEADER_BYTES - offset - len(magic))

    expect("plain documentation", b"# Title\n\nA paragraph.\n", should_fail=False)
    expect(
        "source file",
        b"int main() {\n    return 0;\n}\n",
        should_fail=False,
        filename="main.cpp",
    )
    expect("ncsd image", padded(b"NCSD", 0x100), should_fail=True, filename="image.dat")
    expect("ncch content", padded(b"NCCH", 0x100), should_fail=True, filename="part.dat")
    expect("cro module", padded(b"CRO0", 0x080), should_fail=True, filename="mod.dat")
    expect("romfs blob", padded(b"IVFC", 0x000), should_fail=True, filename="fs.dat")
    expect("denied extension", b"anything\n", should_fail=True, filename="code.bin")
    expect("binary content", b"text\x00more\n", should_fail=True, filename="blob.dat")
    expect(
        "oversized file",
        b"a" * (MAX_TRACKED_FILE_BYTES + 1),
        should_fail=True,
        filename="big.txt",
    )
    expect(
        "high entropy payload",
        bytes(range(256)) * (MIN_BYTES_FOR_ENTROPY_CHECK // 256 + 1),
        should_fail=True,
        filename="payload.dat",
    )
    expect(
        "disassembly listing",
        b"".join(
            b"%08x: e92d4ff0 push {r4, r5}\n" % (0x100000 + index * 4)
            for index in range(MIN_SUSPICIOUS_LINES)
        ),
        should_fail=True,
        filename="listing.txt",
    )
    expect(
        "hex dump",
        b"de ad be ef de ad be ef de ad\n" * MIN_SUSPICIOUS_LINES,
        should_fail=True,
        filename="dump.txt",
    )
    expect(
        "address table in documentation",
        b"| 0x100 | 4 | Magic 'NCCH' |\n" * 20,
        should_fail=False,
        filename="ncch.md",
    )

    print(f"\n{'FAILED' if failures else 'PASSED'}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
