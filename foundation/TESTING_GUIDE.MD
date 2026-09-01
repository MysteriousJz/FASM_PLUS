# Testing Guide

How to write, run, and debug tests in the FASM_PLUS foundation library.

## Running the suite

```sh
foundation/build.sh test     # build everything and run the smoke suite
```

or manually:

```sh
fasm/fasm.x64 foundation/TEST_RUNNER.ASM foundation/build/test_runner
./foundation/build/test_runner
echo $?                       # 0 = all pass
```

A passing run ends with:

```
tests: 47 failed: 0
```

A failing run prints one line per failure and a non-zero summary:

```
FAIL mem_copy_roundtrip line 118
tests: 47 failed: 1
```

The number after `line` is the source line of the failing assertion.

## Writing a test

A test is a function defined with `TEST_DEF` / `TEST_END` that returns
`rax = E_OK` on success. Assertions unwind to the test's `.epilogue` on
failure, so a test never needs its own error-handling boilerplate.

```asm
TEST_DEF my_feature
    ; arrange
    mov     rax, 40
    add     rax, 2
    ; assert
    ASSERT_EQ rax, 42
    ; pass
    mov     rax, E_OK
TEST_END
```

Three steps register it:

1. **`SMOKE_TESTS.ASM`** — write the `TEST_DEF` (as above).
2. **`SMOKE_DATA.INC`** — add a display name:
   ```asm
   tn$my_feature  db 'my_feature',0
   ```
3. **`TEST_RUNNER.ASM`** — add one line to the registry:
   ```asm
   REGISTER_TEST tn$my_feature, test$my_feature
   ```

`FP_TEST_TOTAL` is computed from the registry's extent, so the count stays
correct automatically.

## Assertion reference

| Macro | Passes when |
|-------|-------------|
| `ASSERT_EQ reg, val` | `reg == val` |
| `ASSERT_NE reg, val` | `reg != val` |
| `ASSERT_GT reg, bound` | `reg > bound` (signed) |
| `ASSERT_LT reg, bound` | `reg < bound` (signed) |
| `ASSERT_IN_RANGE reg, lo, hi` | `lo <= reg <= hi` |
| `ASSERT_ZERO reg` | `reg == 0` |
| `ASSERT_NOT_ZERO reg` | `reg != 0` |
| `ASSERT_MEM_EQ a, b, n` | `n` bytes at `a` and `b` are identical |

All assertions preserve the test's flow on success and jump to `.epilogue`
with `rax = E_ASSERTION_FAILED` on failure.

## Interpreting results

- `FAIL <name> line <n>` — open the test file at line `n`; the assertion there
  failed. The operands in the assertion tell you what was expected.
- `tests: T failed: F` — `F` of `T` registered tests failed.
- Exit code `3` (`EXIT_CRASH`) — a test faulted (segfault/illegal instruction/
  bus error). The crash detector printed a `CRASH sig=.. rip=0x.. addr=0x..`
  line to **stderr** before halting; the register snapshot is in
  `fp$crash_regs`.

## Debugging a failure

1. **Reproduce in isolation.** Build a tiny program that calls only the code
   under test (see `/tmp` scratch programs in BOOTSTRAP_LOG.MD for examples).
2. **Print state.** Use `fp$debug_hex value, label` to dump a labelled hex
   value to stderr, or `fp$print_uint` / `fp$print_hex` for stdout.
3. **Check the contract.** Most failures are a caller relying on something the
   contract did not guarantee (a clobbered register, a short count).
4. **Guard pages.** If you suspect a buffer overrun, allocate the buffer with
   `fp$guard_map` so the overflow faults at the boundary instead of silently
   corrupting a neighbor.

## Adding a new test category

For a whole new layer (e.g. data structures):

1. Create `DATA_STRUCTURES.INC` depending only on lower layers.
2. Add its tests to `SMOKE_TESTS.ASM` (or a new `*_TESTS.ASM` included by the
   runner).
3. Register every test in `TEST_RUNNER.ASM`.
4. Keep the rules: independent, deterministic, isolated, self-reporting.

## Testing the Kind Machine extension (Layers 1.5–5.5)

The extension layers add compile-time machinery alongside runtime code, so
their tests split into two kinds:

**Compile-time checks** run at assembly time and fail the *build*, not a test.
`TOKEN_IS_UNIQUE`, `LEXER_TABLE_AUDIT`, and the `fib_ladder` monotonic audit
are examples — if they fire, `build.sh` reports an assembler error naming the
macro and line. There is nothing to register for these; including the file
and emitting a table *is* the test. To check one negatively (prove it catches
a bad table), build a throwaway file in `/tmp` with the violation and confirm
the assembler rejects it.

**Runtime tests** follow the normal three-step registration. Patterns used by
the existing suite:

- **Bit math** — feed known values (`POPCOUNT` of `0xF0F0` = 8; `lzcnt(0)` =
  64, which `bsr` cannot express). Prefer the zero/edge inputs that
  distinguish the single-instruction forms from their undefined-at-zero
  predecessors.
- **Lexer** — drive `fp$lexer_byte` one byte at a time and assert the exact
  token sequence: a full pattern (ELF magic → category token), a byte that is
  no pattern's prefix (raw passthrough), a partial pattern then `fp$lexer_flush`
  (drains as raw tokens), and a non-pattern byte ahead of a pattern (suffix
  demotion). Call `fp$lexer_init` first to isolate the state machine.
- **Comparator** — cover the three sentinels (equal → 0; zero operand or sign
  mismatch → 999) plus known ratios whose expected rung you compute offline
  (`gap = ((max−min)<<32)/min`, then the first ladder entry ≥ gap). Boundary
  cases sit exactly on a rung.
- **Sparse / attention** — build a sparse row by hand with `SPARSE_ROW_APPEND`,
  assert the stored count/indices/values, then verify iteration order and the
  accumulated sums. `WEIGHT_APPLY` and `ATTENTION_PRUNE_POSITION` are tested
  against small fixed matrices/score rows with hand-computed expectations.

Action macros for `MASK_ITERATE`/`SPARSE_ROW_ITERATE` are expanded once per
element with the index in `rcx` (and, for rows, the row in a register pair you
choose); keep them tiny and preserve `rcx`.

## Regression practices

- Run `foundation/build.sh test` before every commit.
- A bug fix must add a test that fails before the fix and passes after.
- Never delete or weaken a test to make the suite pass.
- Keep tests deterministic: no reliance on timing, addresses, or environment
  beyond what the bootstrap verifies.
