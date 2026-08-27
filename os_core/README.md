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
TEST fiber_tree_dependency_chain... PASS
TEST fiber_tree_root_unload... PASS
TEST fiber_tree_root_reload... PASS
TEST fiber_tree_hot_replace... PASS
SUMMARY 12/12
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
| `CORDIS.INC`        | Cordis layer: Context Registry, effects, coeffects, fibers  |
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

## The Cordis layer — dynamic composability

The kernel is not just a platform for user programs; it is the **substrate for
dynamic composability**. Every component — a *fiber* — can be loaded when its
dependencies are satisfied, unloaded when they disappear, and reverted when
unloaded. `CORDIS.INC` implements this with three ideas:

1. **Revertible effects** — every state-changing operation is paired with an
   inverse function. When a fiber unloads, its inverses run in LIFO order,
   returning the system to its pre-fiber state.
2. **Declared dependencies** — a fiber declares what it needs (*inject*) and
   what it contributes (*provide*). The kernel tracks current provisions; when
   a provider disappears, dependents are deactivated automatically.
3. **The Context is first-class** — all state (fibers, provisions, effects)
   lives in one fixed-size **Context Registry** in BSS. No dynamic allocation.

### Context Registry layout

Fixed-size, 64-byte aligned, zero-initialized at boot by `fp$cordis_init`:

```
fp$cordis_fibers[64]      fiber entries: state, id, inject[] / provide[]
                          lists, provide_val[], and a LIFO effect stack head
fp$cordis_coeffects[128]  current provisions: key -> { provider, value }
fp$cordis_effects[256]    revertible operations: fiber, inverse fn, context
```

Keys are 64-bit integer tokens supplied by the caller, so resolution is a
single `cmp` and never divides.

### Cordis syscalls

Added to the dispatch table (numbers 0–7 unchanged):

| #  | Name              | Arguments                          | Returns            |
|----|-------------------|------------------------------------|--------------------|
| 8  | `effect_begin`    | RDI = fiber                        | RAX = 0            |
| 9  | `effect_commit`   | —                                  | RAX = 0            |
| 10 | `effect_rollback` | —                                  | RAX = 0            |
| 11 | `coeffect_declare`| RDI = fiber, RSI = key             | RAX = 0 / errno    |
| 12 | `coeffect_provide`| RDI = fiber, RSI = key, RDX = value| RAX = 0 / errno    |
| 13 | `coeffect_resolve`| RDI = fiber                        | RAX = 0 / E_NOT_FOUND |
| 14 | `fiber_load`      | RDI = fiber                        | RAX = 0 / E_NOT_FOUND |
| 15 | `fiber_unload`    | RDI = fiber                        | RAX = 0 / E_BUSY   |

### Fiber lifecycle

```
Inactive → Loading → Active → Unloading → Inactive
               ↓
             Failed
```

- `fiber_load` marks the fiber Loading, resolves its inject list against the
  coeffect table, and marks it Active only when every dependency is satisfied.
- `fiber_unload` is **two-phase**: *Leave* withdraws the fiber's provisions
  (which cascades — `fp$cordis_deactivate_unsatisfied` deactivates any fiber
  whose dependencies no longer resolve), then *Unload* reverts the fiber's
  effects and marks it Inactive.
- **Deactivation guard** (`fp$fiber_relied`): a fiber is not fully unloaded
  while any Active/Loading fiber relies on a key it provides. The syscall
  returns `E_BUSY` in that case.

### The fiber-tree test

`TEST fiber_tree_*` builds the tree `A (provides X) → B (injects X, provides
Y) → C (injects X, Y)` and proves: the dependency chain activates, unloading
the root provider cascades and reverts effects, reloading restores the tree,
and hot-replacing A with A′ rebinds dependents to the new value.

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
