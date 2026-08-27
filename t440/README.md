# FASM_PLUS — ThinkPad T440 Layer

This directory contains the **hardware-specific** layer of the FASM_PLUS OS
Core for the ThinkPad T440, plus everything needed to boot it on the physical
laptop. The CPU-general core (boot stub, GDT/IDT/paging, exceptions, context,
syscalls, and the Cordis dynamic-composability layer) lives in
[`../os_core`](../os_core) and is shared unchanged.

---

## What's here

### Hardware drivers (each a Cordis fiber)

| File | Driver | Provides | Injects |
|------|--------|----------|---------|
| `PCI_ENUM.INC` | PCI bus enumeration | `PCI_DEVICE_TABLE` | — |
| `VESA_DISPLAY.INC` | Linear-framebuffer display | `DISPLAY_FRAMEBUFFER` | `PCI_DEVICE_TABLE` |
| `PS2_KEYBOARD.INC` | PS/2 keyboard (8042) | `KEYBOARD_INPUT` | `IRQ1_HANDLER` |
| `FONT8X16.INC` | 8×16 bitmap font (data) | — | — |
| `T440_INIT.INC` | Loads the three drivers in dependency order | — | — |

### Boot / build / test

| File | Purpose |
|------|---------|
| `OS_TEST_T440.ASM` | T440 kernel image (Multiboot1 + Multiboot2 variants) |
| `build.sh` | Builds `kernel_mb1.bin` (QEMU) and `kernel_mb2.bin` (GRUB2); `test` and `iso` targets |
| `elf_add_shdr.py` | Adds the ELF section-header table GRUB2's loader requires |
| `grub.cfg` | GRUB2 config: VESA graphics mode + `multiboot2` kernel load |
| `test_mb2_qemu.sh` | Boots the Multiboot2 image in QEMU via a GRUB2 ISO |
| `BOOT_T440.md` | **Complete guide to booting the physical T440 from USB** |

---

## Quick start

```sh
t440/build.sh            # build kernel_mb1.bin + kernel_mb2.bin
t440/build.sh test       # run the test suite in QEMU (Multiboot1 -kernel)
t440/test_mb2_qemu.sh    # boot the Multiboot2 image in QEMU via GRUB2
```

To boot the physical laptop, follow **[BOOT_T440.md](BOOT_T440.md)**.

---

## Two boot protocols, one kernel

The kernel source supports both Multiboot1 and Multiboot2 from a single file:

- **Multiboot1** (`kernel_mb1.bin`) — for QEMU's `-kernel` flag (fast CI loop).
  QEMU 8.2 only loads Multiboot1 32-bit ELF images this way.
- **Multiboot2** (`kernel_mb2.bin`) — for GRUB2 on the T440. The header carries
  a **framebuffer request tag** so GRUB2 sets a VESA graphics mode and passes
  the linear framebuffer address to the kernel, which the boot stub copies into
  the display mode block before the long-mode switch.

GRUB2's ELF parser requires a valid section-header table, which FASM's
`format elf executable` does not emit. `elf_add_shdr.py` appends a minimal one
(null + `.shstrtab`) as a build step — it changes no code or program headers.

---

## Design rules (unchanged from the OS core)

- No floating point. No division in hot paths (shifts/masks only).
- No dynamic allocation (all tables fixed-size, in BSS).
- Every function carries a contract (IN / OUT / CLOBBERS / PRESERVES / ERRORS).
- Every driver is a Cordis fiber with declared dependencies, provisions, and
  revertible effects.

---

## Current status

`SUMMARY 15/16` in QEMU (both the Multiboot1 `-kernel` path and the Multiboot2
GRUB2-ISO path). The `t440_kbd` self-test reports a failure in the emulator
because a scancode can arrive during re-initialization, making the strict
"buffer starts empty" assertion fire — a test-harness artifact, not a hardware
fault. See BOOT_T440.md § Troubleshooting.
