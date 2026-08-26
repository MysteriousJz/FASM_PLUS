# FASM_PLUS OS Core

A minimal 64-bit x86-64 kernel written entirely in FASM assembly, built on the
[FASM_PLUS foundation library](../foundation/README.MD). It boots from a
Multiboot-compliant loader into long mode, brings up the CPU-general
subsystems, prints a heartbeat to the serial port, and runs a self-contained
integration test suite.

No libc. No external libraries. Only FASM and the bare metal.

---

## What it does

When the kernel boots under QEMU you see the heartbeat on the serial line:

```
3
```

That single character is the signal that the whole bootstrap worked: the
loader handed control to the 32-bit stub, the identity page tables were built,
PAE and long mode were enabled, paging was turned on, the 64-bit GDT was
loaded, the CPU far-jumped into 64-bit code, and the serial port came alive.

After the heartbeat, the kernel initializes every subsystem and runs the
integration tests:

```
OS_CORE BOOT OK
3
TEST boot... PASS
TEST serial... PASS
TEST gdt... PASS
TEST idt... PASS
TEST paging... PASS
TEST exception... PASS
TEST context... PASS
TEST syscall... PASS
SUMMARY 8/8
```

---

## Layout

| File                | Role                                                        |
|---------------------|-------------------------------------------------------------|
| `OS_TEST.ASM`       | Top-level kernel image: format, entry, sections, data, BSS  |
| `OS_CORE.INC`       | Includes every component in dependency order (with guards)  |
| `OS_TESTS.INC`      | The heartbeat + the 8 integration tests (`fp$run_all_tests`)|
| `BOOT_STUB.ASM`     | Multiboot header + 32-bit→64-bit long-mode switch           |
| `SERIAL.INC`        | 16550 UART driver (COM1, 115200 8N1)                        |
| `GDT.INC`           | Global Descriptor Table + TSS                               |
| `IDT.INC`           | Interrupt Descriptor Table                                  |
| `PAGING.INC`        | Identity-mapped page tables (first 4 GB, 2 MB pages)        |
| `EXCEPTIONS.INC`    | Exception handlers (vectors 0–31) with full reporting       |
| `CONTEXT.INC`       | Task context save / restore / switch                        |
| `SYSCALL.INC`       | SYSCALL/SYSRET interface and dispatch table                 |
| `KERNEL_INIT.INC`   | Bootstrap sequence + boot banner                            |
| `KERNEL_TEST.INC`   | Test-runner primitives (report, pass/fail, summary, exit)   |
| `build.sh`          | Assemble and boot/test helper                               |

The foundation-layer macros (`CONSTANTS`, `CONTRACTS`, `STATE_MANAGEMENT`,
`BIT_MATH`, `CONTROL_FLOW`) live in [`../foundation`](../foundation) and are
pulled in by `OS_CORE.INC`.

---

## Build

Assemble the kernel with the in-repo FASM:

```sh
os_core/build.sh
```

or directly:

```sh
fasm OS_TEST.ASM kernel.bin
```

The output is a 32-bit ELF executable loaded at `0x100000` (1 MB).

---

## Boot and test

### Heartbeat only

```sh
os_core/build.sh boot
```

equivalent to:

```sh
qemu-system-x86_64 -kernel kernel.bin -serial stdio -display none -no-reboot
```

Expect `OS_CORE BOOT OK`, then `3`, then the test lines. (Use `-display none`
because the kernel has no video driver; the serial line is the only console.)

### Full integration suite

```sh
os_core/build.sh test
```

equivalent to:

```sh
qemu-system-x86_64 -kernel kernel.bin -serial stdio -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot
```

The test runner writes a result code to the QEMU `isa-debug-exit` device on
port `0xF4`. QEMU transforms the written value `V` into exit status
`(V << 1) | 1`, so:

| Runner writes | QEMU exit status | Meaning        |
|---------------|------------------|----------------|
| `0x00`        | `1`              | all tests pass |
| `0x01`        | `3`              | a test failed  |

`build.sh test` decodes this back to the conventional `0` = pass, `1` = fail.

---

## Boot protocol note

The OS core is written to the **Multiboot2** specification. The CI emulator
(QEMU 8.2) only loads `-kernel` images that are **32-bit ELF** carrying an
original **Multiboot (v1)** header — it rejects ELF64-class images and PVH ELF
notes. This image is therefore assembled as a 32-bit ELF with a Multiboot1
header so `qemu-system-x86_64 -kernel` boots it directly.

The kernel proper is pure 64-bit long-mode code; the ELF class constrains only
the container, not the instruction encodings. `BOOT_STUB.ASM` accepts both the
Multiboot1 (`0x2BADB002`) and Multiboot2 (`0x36D76289`) entry magics. To emit a
Multiboot2 header instead (for a v2 loader such as GRUB2), define
`FP_MB2_HEADER = 1` before including `OS_CORE.INC`.

---

## Memory map

```
0x70000               kernel stack top (grows down)
0x90000-0x98FFF       page tables: PML4, PDPT0-3, PD0-3 (9 pages)
0x100000              kernel ELF load address and entry point (_start)
0x00000000-0xFFFFFFFF identity-mapped first 4 GB (2 MB large pages)
```

---

## Design rules

- **No floating point** anywhere.
- **No division** in hot paths — shifts, masks, and repeated subtraction only.
- **Every component carries a documented contract** (IN / OUT / CLOBBERS /
  PRESERVES / ERRORS) in its source file.
- **Revertible effects, declared dependencies, context as a first-class
  entity** — the tests are modular, self-contained units, structured so the
  future Cordis substrate can load and unload components at runtime.
