#!/bin/sh
# =============================================================================
# FASM_PLUS T440 — test_mb2_qemu.sh
# Test the Multiboot2 kernel image in QEMU exactly the way GRUB2 on a USB stick
# will boot it: build a GRUB2 rescue ISO containing the kernel and grub.cfg,
# then boot QEMU from that ISO.
#
# QEMU 8.2's `-kernel` flag does NOT support Multiboot2 (it only loads
# Multiboot1 32-bit ELF images), so the GRUB2-ISO path is the way to exercise
# the Multiboot2 image in the emulator. On newer QEMU builds that accept a
# Multiboot2 `-kernel`, you can point `-kernel` at kernel_mb2.bin directly.
#
# Usage:
#   t440/test_mb2_qemu.sh           # build ISO, boot it headless, print serial
#   t440/test_mb2_qemu.sh display   # same, but show the VGA window (the
#                                   #   on-screen '3' appears there)
#
# Requirements: grub-mkrescue, xorriso, mtools, qemu-system-x86_64.
# =============================================================================
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
QEMU="${QEMU:-qemu-system-x86_64}"

# Build the Multiboot2 image and the ISO (idempotent).
sh "$HERE/build.sh" iso

ISO="$HERE/build/t440_mb2.iso"

echo "booting Multiboot2 image via GRUB2 ISO: $ISO"
echo "(watch for 'OS_CORE BOOT OK', the '3' heartbeat, and the test summary)"

if [ "${1:-}" = "display" ]; then
    # show the graphics window so the framebuffer '3' is visible
    exec "$QEMU" -cdrom "$ISO" -serial stdio -no-reboot
else
    exec "$QEMU" -cdrom "$ISO" -serial stdio -display none -no-reboot
fi
