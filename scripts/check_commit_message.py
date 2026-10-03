#!/usr/bin/env python3
"""Check a commit message against Hexlith's history rules.

Commit messages carry no AI attribution. Hexlith discloses AI assistance once,
at project level, in README.md, so a per-commit trailer adds noise without
adding information. Several tools insert one by default, which is why this is a
hook rather than a note. HEXLITH_ALLOW_AI_ATTRIBUTION=1 skips that one check for
a commit; the rest still run.

The remaining rules are ordinary commit hygiene: a short imperative subject, a
blank line before the body, and no placeholder commits, because the history has
to stay bisectable.

Run the built-in tests with --self-test.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# Skips the attribution check for a single commit.
ATTRIBUTION_OPT_OUT = "HEXLITH_ALLOW_AI_ATTRIBUTION"

MAX_SUBJECT_LENGTH = 72

# Trailers and sign-off lines that name a coding assistant. Matching the
# assistant by name, rather than matching Co-Authored-By itself, keeps
# genuine human co-authorship working.
ASSISTANT_NAMES = r"(?:claude|anthropic|copilot|chatgpt|openai|gemini|cursor|codeium|devin|aider)"

FORBIDDEN_PATTERNS = (
    (
        re.compile(rf"^\s*co-authored-by:.*{ASSISTANT_NAMES}", re.IGNORECASE | re.MULTILINE),
        "a Co-Authored-By trailer naming a coding assistant",
    ),
    (
        re.compile(r"noreply@anthropic\.com", re.IGNORECASE),
        "an assistant's email address",
    ),
    (
        re.compile(rf"generated\s+(?:with|by)\b.*{ASSISTANT_NAMES}", re.IGNORECASE),
        "a 'generated with' line",
    ),
    (
        re.compile(rf"^\s*{ASSISTANT_NAMES}[-\s]session\b", re.IGNORECASE | re.MULTILINE),
        "a session identifier trailer",
    ),
    (
        re.compile(rf"^\s*(?:assisted|written)-by:.*{ASSISTANT_NAMES}", re.IGNORECASE | re.MULTILINE),
        "an assistant attribution trailer",
    ),
    (
        re.compile("\N{ROBOT FACE}"),
        "a robot emoji, which tools use to mark generated text",
    ),
)

# Subjects that say nothing about what changed. The history is a sequence of
# atomic increments, so there is no place for these: amend or fix up instead.
PLACEHOLDER_SUBJECT = re.compile(
    r"^(?:wip\b|fixup!|squash!|temp\b|tmp\b|asdf|test commit\b"
    r"|fix(?:es|ed)?\s+(?:previous|last|above)\s+commit)",
    re.IGNORECASE,
)


def strip_comments(raw_message: str) -> str:
    """Remove the comment lines git adds to the message template."""
    return "\n".join(
        line for line in raw_message.splitlines() if not line.startswith("#")
    ).strip("\n")


def check_message(raw_message: str, *, allow_attribution: bool = False) -> list[str]:
    """Return every rule the message breaks, as readable sentences."""
    message = strip_comments(raw_message)
    problems = []

    if not allow_attribution:
        for pattern, description in FORBIDDEN_PATTERNS:
            if pattern.search(message):
                problems.append(
                    f"The message contains {description}. Hexlith discloses AI "
                    "assistance in README.md, not per commit. Remove the line, "
                    f"or set {ATTRIBUTION_OPT_OUT}=1 to keep it."
                )

    lines = message.splitlines()
    if not lines or not lines[0].strip():
        problems.append("The message has no subject line.")
        return problems

    subject = lines[0].rstrip()

    if len(subject) > MAX_SUBJECT_LENGTH:
        problems.append(
            f"The subject is {len(subject)} characters, over the "
            f"{MAX_SUBJECT_LENGTH} character limit."
        )

    if subject.endswith("."):
        problems.append("The subject ends with a full stop; drop it.")

    if subject[0].islower():
        problems.append("The subject starts with a lowercase letter.")

    if PLACEHOLDER_SUBJECT.match(subject):
        problems.append(
            f"'{subject}' is a placeholder subject. Every commit has to stand "
            "on its own, so amend or fix up the earlier commit instead."
        )

    if len(lines) > 1 and lines[1].strip():
        problems.append("The subject and the body need a blank line between them.")

    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("message_file", nargs="?", type=Path)
    parser.add_argument(
        "--revision-range",
        help="check every commit message in a range, for use in CI",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run the checker's own tests"
    )
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        return self_test()

    if arguments.revision_range:
        return check_revision_range(arguments.revision_range)

    if arguments.message_file is None:
        parser.error("a message file, --revision-range or --self-test is required")

    problems = check_message(
        arguments.message_file.read_text(encoding="utf-8"),
        allow_attribution=attribution_allowed(),
    )
    report(problems)
    return 1 if problems else 0


def attribution_allowed() -> bool:
    return os.environ.get(ATTRIBUTION_OPT_OUT, "") not in ("", "0")


def check_revision_range(revision_range: str) -> int:
    revisions = subprocess.run(
        ["git", "rev-list", revision_range],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()

    failed = False
    for revision in revisions:
        message = subprocess.run(
            ["git", "log", "-1", "--format=%B", revision],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        problems = check_message(message, allow_attribution=attribution_allowed())
        if problems:
            failed = True
            subject = message.splitlines()[0] if message.strip() else "(empty)"
            print(f"\n{revision[:12]} {subject}", file=sys.stderr)
            report(problems)
    return 1 if failed else 0


def report(problems: list[str]) -> None:
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)


def self_test() -> int:
    failures = 0

    def expect(name: str, message: str, should_fail: bool) -> None:
        nonlocal failures
        problems = check_message(message)
        if bool(problems) != should_fail:
            wanted = "rejected" if should_fail else "accepted"
            print(f"FAIL {name}: expected {wanted}, got {problems}")
            failures += 1
        else:
            print(f"ok   {name}")

    expect("subject only", "Add Gitignore", should_fail=False)
    expect(
        "subject and body",
        "Add Gitignore\n\nExcludes binary payloads wholesale.\n",
        should_fail=False,
    )
    expect(
        "build break trailer",
        "Split the Memory Map Header\n\nUnavoidable mid-refactor.\n\n"
        "Build-Break: the lifter is rewired in the next commit\n",
        should_fail=False,
    )
    expect(
        "human co-author",
        "Add Gitignore\n\nBody.\n\nCo-Authored-By: A Contributor <dev@example.com>\n",
        should_fail=False,
    )
    expect(
        "assistant co-author",
        "Add Gitignore\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n",
        should_fail=True,
    )
    expect(
        "generated with line",
        "Add Gitignore\n\nGenerated with Claude Code\n",
        should_fail=True,
    )
    expect(
        "robot emoji",
        "Add Gitignore\n\n\N{ROBOT FACE} Generated\n",
        should_fail=True,
    )
    expect(
        "session trailer",
        "Add Gitignore\n\nClaude-Session: abc123\n",
        should_fail=True,
    )
    expect("empty message", "", should_fail=True)
    expect("comments only", "# Please enter the commit message\n", should_fail=True)
    expect("lowercase subject", "add gitignore", should_fail=True)
    expect("trailing full stop", "Add Gitignore.", should_fail=True)
    expect("overlong subject", "A" + "b" * MAX_SUBJECT_LENGTH, should_fail=True)
    expect("wip subject", "WIP on the Extractor", should_fail=True)
    expect("fixup subject", "Fix Previous Commit", should_fail=True)
    expect(
        "missing blank line",
        "Add Gitignore\nExcludes binary payloads.\n",
        should_fail=True,
    )

    attributed = "Add Gitignore\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n"
    if check_message(attributed, allow_attribution=True):
        print("FAIL opt-out: attribution still rejected when allowed")
        failures += 1
    else:
        print("ok   opt-out honours HEXLITH_ALLOW_AI_ATTRIBUTION")

    print(f"\n{'FAILED' if failures else 'PASSED'}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
