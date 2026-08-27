#!/bin/sh
# =============================================================================
# FASM_PLUS T440 — build.sh
# Assembles the T440 kernel in both boot-protocol variants and (optionally)
# boots/tests them in QEMU.
#
# Outputs (into t440/build/):
#   kernel_mb1.bin   Multiboot1 image — for `qemu-system-x86_64 -kernel`
#   kernel_mb2.bin   Multiboot2 image — for GRUB2 on the physical T440
#                    (section-header table patched in for GRUB2's ELF parser)
#
# Usage:
#   t440/build.sh             # build both images
#   t440/build.sh test        # build, then run the QEMU test suite (MB1)
#   t440/build.sh iso         # build, then make a GRUB2 rescue ISO (MB2)
#
# Requirements: the in-repo FASM (fasm/fasm), python3 (for the ELF
# section-header fixup), and for the iso target grub-mkrescue + xorriso +
# mtools. qemu-system-x86_64 for test.
# =============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
FASM="$ROOT/fasm/fasm"
OUT="$HERE/build"
KERNEL_MB1="$OUT/kernel_mb1.bin"
KERNEL_MB2="$OUT/kernel_mb2.bin"
QEMU="${QEMU:-qemu-system-x86_64}"

mkdir -p "$OUT"

# --- Multiboot1 image (QEMU -kernel) -----------------------------------------
echo "assembling OS_TEST_T440.ASM (Multiboot1) -> $KERNEL_MB1"
"$FASM" "$HERE/OS_TEST_T440.ASM" "$KERNEL_MB1"

# --- Multiboot2 image (GRUB2) ------------------------------------------------
echo "assembling OS_TEST_T440.ASM (Multiboot2) -> $KERNEL_MB2"
"$FASM" -dFP_MB2_HEADER=1 "$HERE/OS_TEST_T440.ASM" "$KERNEL_MB2"

# GRUB2's ELF parser rejects the section-header-less image FASM emits
# ("invalid section header table offset in e_shoff"). Append a minimal valid
# section-header table so GRUB2 accepts the kernel.
echo "patching section-header table into $KERNEL_MB2 (for GRUB2)"
python3 "$HERE/elf_add_shdr.py" "$KERNEL_MB2" "$KERNEL_MB2"

# --- verify the Multiboot2 magic is present in the first 8 KB ----------------
if ! python3 - "$KERNEL_MB2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read(8192)
sys.exit(0 if b'\xd6\x50\x52\xe8' in d else 1)
PY
then
    echo "error: Multiboot2 magic not found in the first 8 KB of $KERNEL_MB2" >&2
    exit 1
fi

# --- decode QEMU isa-debug-exit status ---------------------------------------
run_qemu() {
    set +e
    timeout 30 "$QEMU" -kernel "$KERNEL_MB1" -serial stdio -display none \
        -no-reboot $1
    rc=$?
    set -e
    return $rc
}

case "${1:-}" in
    test)
        echo "running QEMU test suite (Multiboot1 image)"
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
    iso)
        echo "building GRUB2 rescue ISO with the Multiboot2 kernel"
        ISO="$OUT/t440_mb2.iso"
        STAGE="$OUT/iso"
        mkdir -p "$STAGE/boot/grub"
        cp "$KERNEL_MB2" "$STAGE/boot/kernel.bin"
        cp "$HERE/grub.cfg" "$STAGE/boot/grub/grub.cfg"
        grub-mkrescue -o "$ISO" "$STAGE"
        echo "ISO written to $ISO"
        echo "test it with:"
        echo "  $QEMU -cdrom $ISO -serial stdio -display none -no-reboot"
        ;;
esac

echo "build complete"
