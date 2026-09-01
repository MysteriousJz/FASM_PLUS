# FASM_PLUS Foundation Library

A layered, self-verifying macro library that treats FASM (Flat Assembler) as a
**platform construction kit** — not just an assembler. The foundation provides
rigorous systems-development primitives for x86-64 Linux: explicit contracts,
verified state management, safe memory operations, structured control flow,
direct syscall I/O, a bootstrap sequence, a crash detector, and a test
framework that proves every layer before any higher layer may rely on it.

No libc. No external libraries. Only FASM syntax and the Linux kernel ABI.

---

## Philosophy

1. **Literal Transparency** — every abstraction peels back to the actual
   machine instructions. The macros emit the obvious cmp/jcc/mov sequences;
   nothing is hidden. You can always read the disassembly and recognize the
   source.

2. **Contract-Based Design** — every macro and function carries a documented
   contract: what registers it reads and writes, what memory it touches, what
   preconditions hold, what postconditions are guaranteed, and what errors are
   possible. See [CONTRACTS.MD](CONTRACTS.MD).

3. **Bootstrap Verification** — reliability is built from nothing. Each layer
   is tested in isolation before any higher layer uses it. The smoke suite
   (`SMOKE_TESTS.ASM`) is the proof; a layer is not "done" until its tests
   pass.

4. **Crash Prevention Without Explanation** — silent corruption is the worst
   failure mode in assembly. Every component is built so failures are
   *detectable* and *localizable*. We prefer a controlled halt with preserved
   state over an undefined crash. The crash detector captures the faulting
   RIP, fault address, and full register snapshot, then halts with a dedicated
   exit code.

5. **Compositional Building** — each macro is built from smaller, already-
   verified macros. No monolithic, untestable implementations.

---

## Bit Math Philosophy

The Kind Machine extension (Layers 1.5+) treats bit-level operations as the
**default**, not an optimization. This is a fundamental design philosophy:

- **No division when bit shifts suffice** — all power-of-2 operations use
  `shr`/`shl`/`sar`.
- **No multiplication when bit tricks work** — `lea` for ×2/×4/×8, shifts for
  ×2^n; `imul` is reserved for genuine non-power multiplies (e.g. PREMIX).
- **No modulo when bitwise AND works** — `x mod 2^n = x and (2^n - 1)`.
- **No floating point anywhere** — fixed-point integer arithmetic only
  (the Fibonacci ladder is Q32.32).
- **No branches in hot paths** — `cmov`/`setcc` branchless forms; the
  comparator's min/max is branch-free.
- **Table lookups via bit extraction** — `shr` + `and` decode packed fields;
  token range/offset and lexer buffer bytes are extracted, never divided.

This is not premature optimization — it is mechanical sympathy. The CPU
executes these operations in a single cycle with perfect pipelining, and the
resulting assembly is *obviously* what the macro source describes (Literal
Transparency). Every abstraction in the extension layers decomposes to these
primitives.

## Architecture: Layers

```
Layer 6    TEST_FRAMEWORK.INC   assertions, test definition, run-all driver
Layer 5.5  ATTENTION_SCAFFOLDING.INC  PREMIX_2x2, WEIGHT_APPLY, prune loop
Layer 5    SPARSE_STRUCTURES.INC      MASK_* bit ops, SPARSE_ROW primitives
Layer 4.5  FIBONACCI.INC              fib_ladder (phi^n Q32.32), closecheck
Layer 4    IO_BASIC.INC         console + file I/O on raw syscalls
           BOOTSTRAP.INC        init/shutdown, CPUID, stack verification
           CRASH_DETECTOR.INC   signal handlers, context capture, post-mortem
Layer 3    CONTROL_FLOW.INC     FOR/WHILE/REPEAT loops, guards, cleanup
Layer 2.5  TOKEN_MANAGEMENT.INC token vocabulary (collision-free, packed)
           LEXER.INC            static pattern-matching lexer (byte->token)
Layer 2    MEMORY_PRIMITIVES.INC memzero/copy/compare, bump allocator, guard pages
Layer 1.5  BIT_MATH.INC         popcount/lzcnt/tzcnt, log2, branchless min/max
Layer 1    CONTRACTS.INC        compile-time static assertions
           STATE_MANAGEMENT.INC register save/restore, frames, canaries
Layer 0    CONSTANTS.INC        sizes, alignment, syscalls, error codes
```

Layers 1.5, 2.5, 4.5, 5, and 5.5 form the **Kind Machine extension**: the
mechanical primitives (token vocabulary, pattern lexer, golden-ratio distance
oracle, sparse structures, attention scaffolding) that a hardware-kind
attention mechanism is assembled from. They obey the same dependency rule and
are governed by the Bit Math Philosophy below.

**Dependency rule:** a layer may only use lower layers. Constants depend on
nothing; the test framework depends on everything below it. The rule is
enforced by the `err` guards at the top of each include file — including a
layer before its dependencies fails the build with a clear message.

### Files

| File | Role |
|------|------|
| `CONSTANTS.INC` | Pure compile-time constants. Emits no code. |
| `CONTRACTS.INC` | `STATIC_ASSERT`, `ASSERT_POWER_OF_TWO`, alignment/range checks. |
| `STATE_MANAGEMENT.INC` | `PUSH_VOLATILE`, `FRAME_ENTER`, `CANARY_PLACE`, alignment. |
| `MEMORY_PRIMITIVES.INC` | `fp$mem_zero/copy/compare`, bump allocator, `fp$guard_map`. |
| `CONTROL_FLOW.INC` | `FOR/NEXT`, `WHILE/WEND`, `REPEAT/UNTIL`, `BREAK`, guard returns. |
| `IO_BASIC.INC` | `fp$print_*`, `fp$read_char`, `fp$file_*`, `fp$mmap_file`, debug output. |
| `BIT_MATH.INC` | `POPCOUNT`, `COUNT_LEADING/TRAILING_ZEROS`, `LOG2_FLOOR/CEIL`, `BIT_MATH_MIN/MAX`. |
| `TOKEN_MANAGEMENT.INC` | `DECLARE_TOKEN_RANGE`, `DEFINE_TOKEN`, `TOKEN_IS_UNIQUE`, `TOKEN_RAW_BYTE`. |
| `LEXER.INC` | `LEXER_ENTRY`/`LEXER_SENTINEL`, `LEXER_TABLE_AUDIT`, `fp$lexer_init/byte/flush`. |
| `FIBONACCI.INC` | `fib_ladder`, `FIBONACCI_LADDER_RANGE`, `FIBONACCI_THRESHOLD`, `fp$closecheck_compare`. |
| `SPARSE_STRUCTURES.INC` | `MASK_SET/CLEAR/TEST_BIT`, `MASK_ITERATE`, `SPARSE_ROW`, `SPARSE_ROW_APPEND/ITERATE`. |
| `ATTENTION_SCAFFOLDING.INC` | `PREMIX_2x2`, `WEIGHT_APPLY`, `ATTENTION_PRUNE_POSITION`. |
| `TEST_FRAMEWORK.INC` | `ASSERT_*`, `TEST_DEF`, `REGISTER_TEST`, `fp$test_run_all`. |
| `BOOTSTRAP.INC` | `fp$init`, `fp$cpuid_check`, `fp$exit`, `fp$halt_error`. |
| `CRASH_DETECTOR.INC` | `fp$crash_install`, `fp$crash_handler`, state snapshot. |
| `FOUNDATION_DATA.INC` | All library state; include once, last. |
| `SMOKE_TESTS.ASM` / `SMOKE_DATA.INC` | One test per public macro. |
| `TEST_RUNNER.ASM` | Registers and runs all tests; exit code encodes outcome. |
| `EXAMPLE.ASM` | A working program using every layer. |
| `build.sh` | Builds and (optionally) runs everything. |

---

## Quick start

```sh
# from the repository root
foundation/build.sh test        # build + run the smoke suite
./foundation/build/test_runner  # run tests; echo $? -> 0 on success
./foundation/build/example      # run the demo program
```

Using the library in your own program (flat ELF64 executable):

```asm
format elf64 executable
entry _start

include 'CONSTANTS.INC'
include 'CONTRACTS.INC'
include 'BIT_MATH.INC'
include 'STATE_MANAGEMENT.INC'
include 'MEMORY_PRIMITIVES.INC'
include 'CONTROL_FLOW.INC'
include 'IO_BASIC.INC'
include 'TOKEN_MANAGEMENT.INC'
include 'LEXER.INC'
include 'FIBONACCI.INC'
include 'SPARSE_STRUCTURES.INC'
include 'ATTENTION_SCAFFOLDING.INC'
include 'TEST_FRAMEWORK.INC'
include 'BOOTSTRAP.INC'
include 'CRASH_DETECTOR.INC'

segment readable executable
_start:
    call    fp$init
    cmp     rax, E_OK
    jne     .fail
    call    fp$crash_install

    mov     rdi, 123
    call    fp$print_uint
    call    fp$print_newline

    mov     edi, EXIT_SUCCESS
    call    fp$exit
.fail:
    mov     edi, EXIT_BOOTSTRAP_FAIL
    call    fp$exit

include 'FOUNDATION_DATA.INC'
; if you use TEST_FRAMEWORK/BOOTSTRAP, the registry symbols must exist:
segment readable writeable
align 8
fp$test_registry_begin:
fp$test_registry_end:
FP_TEST_TOTAL = 0
```

---

## Testing methodology

Every public macro and function has at least one smoke test. Tests are:

- **Independent** — no test depends on another's side effects.
- **Deterministic** — same input, same result, every run.
- **Isolated** — the runner saves/restores non-volatile registers around each.
- **Self-reporting** — a failing assertion records its source line and the
  runner prints `FAIL <name> line <n>`.
- **Crash-contained** — a test that faults triggers the crash detector, which
  captures state and halts with `EXIT_CRASH` instead of corrupting the run.

See [TESTING_GUIDE.MD](TESTING_GUIDE.MD) for how to write and run tests.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | all tests passed |
| 2 | one or more tests failed (`EXIT_TEST_FAILURE`) |
| 3 | a crash was captured (`EXIT_CRASH`) |
| 4 | bootstrap failed (`EXIT_BOOTSTRAP_FAIL`) |

---

## Extending the library

1. Pick the correct layer for your addition (or add a new layer above 6).
2. Write the contract comment **first** — registers, memory, errors.
3. Implement using only lower-layer primitives.
4. Add a `TEST_DEF` in `SMOKE_TESTS.ASM`, a name in `SMOKE_DATA.INC`, and a
   `REGISTER_TEST` line in `TEST_RUNNER.ASM`.
5. Run `foundation/build.sh test`. Do not merge until the suite is green.

Contributions must preserve the dependency rule, keep contracts accurate, and
include tests for both success and error paths.

---

## Requirements & constraints

- x86-64 Linux (kernel ABI used directly via `syscall`).
- The in-repo FASM 1.73.x assembler (`fasm/fasm.x64`).
- No C runtime, no external libraries, no self-modifying code.
- Flat `format elf64 executable` output (absolute addressing; see
  BOOTSTRAP_LOG.MD for the position-independence discussion).
