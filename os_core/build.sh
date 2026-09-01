#!/bin/sh
# =============================================================================
# FASM_PLUS OS Core — build.sh
# Assembles the OS core kernel (OS_TEST.ASM) with the in-repo FASM, and
# optionally boots it in QEMU.
#
# Usage:
#   os_core/build.sh            # assemble kernel.bin into os_core/build/
#   os_core/build.sh boot       # assemble, then boot the heartbeat in QEMU
#   os_core/build.sh test       # assemble, then run the full test suite
#
# Requirements: the in-repo FASM (fasm/fasm) and, for boot/test,
# qemu-system-x86_64. The script is POSIX sh.
# =============================================================================
set -eu

# resolve paths relative to the repository root regardless of the caller's cwd
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
FASM="$ROOT/fasm/fasm"
OUT="$HERE/build"
KERNEL="$OUT/kernel.bin"

QEMU="${QEMU:-qemu-system-x86_64}"

mkdir -p "$OUT"

echo "assembling OS_TEST.ASM -> $KERNEL"
"$FASM" "$HERE/OS_TEST.ASM" "$KERNEL"

# isa-debug-exit encoding: the guest writes a value V to port 0xF4 and QEMU
# exits with status (V << 1) | 1. KERNEL_TEST writes 0 on success and 1 on
# failure, so QEMU exits 1 on success and 3 on failure. Decode it back so the
# script's own exit status is the conventional 0 = pass, 1 = fail.
run_qemu() {
    # $1 = extra QEMU args (e.g. the isa-debug-exit device)
    set +e
    timeout 30 "$QEMU" -kernel "$KERNEL" -serial stdio -display none \
        -no-reboot $1
    rc=$?
    set -e
    return $rc
}

case "${1:-}" in
    boot)
        echo "booting kernel (heartbeat: expect '3' on serial)"
        run_qemu "" || true
        ;;
    test)
        echo "running integration test suite"
        run_qemu "-device isa-debug-exit,iobase=0xf4,iosize=0x04" || rc=$?
        rc=${rc:-0}
        if [ "$rc" -eq 1 ]; then
            echo "RESULT: all tests passed (QEMU exit 1 = pass)"
            exit 0
        elif [ "$rc" -eq 3 ]; then
            echo "RESULT: a test failed (QEMU exit 3 = fail)" >&2
            exit 1
        else
            echo "RESULT: QEMU exited abnormally (status $rc)" >&2
            exit 1
        fi
        ;;
esac

echo "build complete"
