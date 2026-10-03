#!/bin/sh
# Install Hexlith's git hooks. Run once after cloning.
#
# The hooks are the only thing standing between a careless `git add` and
# game-derived content in the history, so this is not optional setup. CI
# repeats the same checks for anyone who skips it.

set -eu

cd "$(git rev-parse --show-toplevel)"

if ! command -v pre-commit > /dev/null 2>&1; then
    echo "pre-commit is not installed. On Fedora: sudo dnf install pre-commit" >&2
    exit 1
fi

pre-commit install --install-hooks

echo
echo "Installed hooks:"
echo "  pre-commit  rejects game-derived content and reformats C++"
echo "  commit-msg  rejects AI attribution and malformed subjects"
