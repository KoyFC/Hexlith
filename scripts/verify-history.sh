#!/usr/bin/env bash
# Build and test every commit in a range, so that the history stays
# bisectable. A `git bisect` is only worth running if every commit it can land
# on compiles.
#
# Each commit is checked out into a throwaway git worktree and built there.
# The obvious alternative, `git rebase --exec`, replays the commits: a failure
# leaves the repository mid-rebase, and a mistake rewrites history. A
# verification tool should not be able to damage what it verifies. The
# worktree approach also leaves the working tree usable while it runs.
#
# A commit carrying a `Build-Break:` trailer is allowed to fail, because the
# commit rules permit one where it is unavoidable. The commit after it is not,
# and this script enforces that.

set -euo pipefail

JOBS=$(nproc)
KEEP_GOING=0
RANGE=""

usage() {
    cat << 'USAGE'
Usage: scripts/verify-history.sh [options] [<revision-range>]

Builds and tests every commit in the range. Defaults to the commits on the
current branch that are not on its upstream, or the whole history when there
is no upstream.

Options:
  -j, --jobs N      parallel build jobs (default: nproc)
  -k, --keep-going  report every failing commit instead of stopping at the first
  -h, --help        show this message

Examples:
  scripts/verify-history.sh
  scripts/verify-history.sh HEAD~10..HEAD
  scripts/verify-history.sh -k main..my-branch
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        -j | --jobs)
            JOBS="$2"
            shift 2
            ;;
        -k | --keep-going)
            KEEP_GOING=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            RANGE="$1"
            shift
            ;;
    esac
done

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

if [ -z "$RANGE" ]; then
    if upstream=$(git rev-parse --abbrev-ref '@{upstream}' 2> /dev/null); then
        RANGE="$upstream..HEAD"
    else
        RANGE="HEAD"
        echo "No upstream is configured, so the whole history will be checked."
    fi
fi

mapfile -t COMMITS < <(git rev-list --reverse "$RANGE")

if [ ${#COMMITS[@]} -eq 0 ]; then
    echo "No commits in range $RANGE, nothing to verify."
    exit 0
fi

WORKTREE=$(mktemp -d -t hexlith-verify-XXXXXX)
BUILD_DIR="$WORKTREE/build"

cleanup() {
    git worktree remove --force "$WORKTREE/source" 2> /dev/null || true
    rm -rf "$WORKTREE"
}
trap cleanup EXIT

git worktree add --detach --quiet "$WORKTREE/source" HEAD

passed=0
skipped=0
failed=0
declare -a FAILED_COMMITS=()
previous_broke_build=0

echo "Verifying ${#COMMITS[@]} commit(s) in $RANGE with $JOBS job(s)."
echo

for commit in "${COMMITS[@]}"; do
    subject=$(git log -1 --format='%s' "$commit")
    short=${commit:0:12}

    git -C "$WORKTREE/source" checkout --quiet --detach "$commit"

    # A commit from before the build existed cannot be built. Reporting it as
    # skipped is honest; claiming it passed is not.
    if [ ! -f "$WORKTREE/source/CMakeLists.txt" ]; then
        printf 'SKIP %s  %s\n' "$short" "$subject"
        printf '       no CMakeLists.txt at this commit\n'
        skipped=$((skipped + 1))
        previous_broke_build=0
        continue
    fi

    allowed_to_break=0
    if git log -1 --format='%B' "$commit" | grep -qE '^Build-Break:'; then
        allowed_to_break=1
    fi

    if [ "$previous_broke_build" -eq 1 ] && [ "$allowed_to_break" -eq 1 ]; then
        printf 'FAIL %s  %s\n' "$short" "$subject"
        printf '       two Build-Break commits in a row; green must be restored immediately\n'
        failed=$((failed + 1))
        FAILED_COMMITS+=("$short $subject")
        [ "$KEEP_GOING" -eq 1 ] || break
        continue
    fi

    log="$WORKTREE/$short.log"
    if cmake -S "$WORKTREE/source" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DHEXLITH_WERROR=ON > "$log" 2>&1 \
        && cmake --build "$BUILD_DIR" -j "$JOBS" >> "$log" 2>&1 \
        && ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error \
            >> "$log" 2>&1; then
        printf 'PASS %s  %s\n' "$short" "$subject"
        passed=$((passed + 1))
        previous_broke_build=0
    elif [ "$allowed_to_break" -eq 1 ]; then
        reason=$(git log -1 --format='%B' "$commit" | sed -n 's/^Build-Break: *//p')
        printf 'BREAK %s %s\n' "$short" "$subject"
        printf '       declared: %s\n' "$reason"
        skipped=$((skipped + 1))
        previous_broke_build=1
    else
        printf 'FAIL %s  %s\n' "$short" "$subject"
        printf '       last 20 lines of %s:\n' "$log"
        tail -20 "$log" | sed 's/^/       | /'
        failed=$((failed + 1))
        FAILED_COMMITS+=("$short $subject")
        previous_broke_build=0
        [ "$KEEP_GOING" -eq 1 ] || break
    fi
done

echo
echo "$passed passed, $skipped skipped, $failed failed"

if [ "$failed" -gt 0 ]; then
    echo
    echo "Failing commits:"
    for entry in "${FAILED_COMMITS[@]}"; do
        echo "  $entry"
    done
    exit 1
fi
