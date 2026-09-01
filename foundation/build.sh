#!/bin/sh
# =============================================================================
# FASM_PLUS Foundation Library — build.sh
# Builds the test runner and the example program using the in-repo FASM.
#
# Usage:
#   foundation/build.sh            # build both into foundation/build/
#   foundation/build.sh test       # build and run the smoke test suite
#
# The script is POSIX sh and requires no tools beyond the in-repo assembler.
# =============================================================================
set -eu

# resolve paths relative to the repository root regardless of the caller's cwd
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
FASM="$ROOT/fasm/fasm.x64"
OUT="$HERE/build"

mkdir -p "$OUT"

echo "assembling TEST_RUNNER.ASM -> $OUT/test_runner"
"$FASM" "$HERE/TEST_RUNNER.ASM" "$OUT/test_runner"

echo "assembling EXAMPLE.ASM -> $OUT/example"
"$FASM" "$HERE/EXAMPLE.ASM" "$OUT/example"

chmod +x "$OUT/test_runner" "$OUT/example"

if [ "${1:-}" = "test" ]; then
    echo "running smoke test suite"
    "$OUT/test_runner"
    echo "test runner exit code: $?"
fi

echo "build complete"
