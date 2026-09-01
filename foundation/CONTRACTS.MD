# Contract Format

Every public macro and function in the foundation library carries a contract.
Contracts are the unit of trust: a caller may rely only on what the contract
guarantees, and a test exists to prove each guarantee.

## The contract block

A contract is a structured comment immediately above the definition:

```
; fp$mem_copy — overlap-safe copy (memmove semantics)
;   CONTRACT
;     IN        : rdi = dest, rsi = src, rdx = byte count
;     OUT       : rdx bytes copied; correct even if ranges overlap
;     CLOBBERS  : rax rcx rsi rdi flags; memory written to dest
;     PRESERVES : rdx r8-r15 and all non-volatile registers
;     ERRORS    : none
```

### Fields

- **IN** — inputs and where they live (register or memory), plus any
  preconditions (alignment, non-null, range).
- **OUT** — results and where they are returned, plus guaranteed
  postconditions.
- **CLOBBERS** — every register, the flags, and any memory the implementation
  may modify. If it is not listed here (or covered by PRESERVES), it must not
  change.
- **PRESERVES** — registers guaranteed unchanged. System V non-volatile
  registers (`rbx rbp r12 r13 r14 r15 rsp`) are preserved by *every* function
  unless explicitly listed as clobbered.
- **ERRORS** — error values returned (in `rax`, following the kernel's small-
  negative convention) or traps raised.

## Register preservation rules

The library follows the System V AMD64 ABI:

- **Volatile (caller-saved):** `rax rcx rdx rsi rdi r8 r9 r10 r11`. A function
  may clobber any of these *only if* its contract says so.
- **Non-volatile (callee-saved):** `rbx rbp r12 r13 r14 r15`. Preserved across
  every library call. Use `PUSH_NONVOLATILE`/`POP_NONVOLATILE` in your own
  code before using them.
- **`syscall` clobbers** `rcx` and `r11` (the kernel uses them for the return
  address and flags). Contracts that issue syscalls list them as clobbered.
- **Flags** are clobbered by almost everything; assume nothing survives a call
  unless the contract says otherwise.

## Memory management rules

- The bump allocator (`fp$alloc*`) never frees individual blocks; it hands out
  forward-moving, 16-aligned slices of a caller-supplied region and can be
  rewound to a `fp$alloc_mark` token with `fp$alloc_rewind`. Exhaustion returns
  `E_OUT_OF_MEMORY`; it never wraps or overruns the region.
- `fp$guard_map` maps `N` usable read/write pages with a `PROT_NONE` guard
  page above them so an overflow faults deterministically at the boundary.
- Red-zone (the 128 bytes below `rsp`) use is permitted only in leaf
  functions. The print helpers use it for scratch; any function that makes a
  further call must not rely on red-zone contents surviving.

## Error handling conventions

- Errors are small negative integers in `rax`, mirroring the kernel. `E_OK`
  is 0; success values are non-negative. Library-private codes start at
  `-1000` (see `CONSTANTS.INC`).
- Syscall results pass through untranslated, so a negative `rax` from a
  wrapper is the kernel's `-errno`.
- **Fail loudly.** Where continuing would corrupt state (stack canary
  mismatch, bootstrap failure), the code halts with a distinctive exit code
  instead of returning an error the caller might ignore.

## Compile-time contracts

`CONTRACTS.INC` enforces statically what can be checked statically:

- `STATIC_ASSERT cond` — any constant expression.
- `ASSERT_POWER_OF_TWO value` — alignment bases.
- `ASSERT_ALIGNMENT value, alignment` — offset/address alignment.
- `ASSERT_IN_RANGE value, low, high` — inclusive bounds.
- `ASSERT_FITS_BYTE/WORD/DWORD value` — truncation guards.

A failed compile-time assertion stops the build and names the macro and line.

## Thread safety

The foundation is single-threaded by design. Global state (the allocator
pointer, test counters, crash snapshot) has no locking. Higher layers that add
concurrency must introduce their own synchronization and update the affected
contracts. The bump allocator is not thread-safe; give each thread its own
region.

## Performance guarantees

- `fp$mem_zero`/`fp$mem_copy` use `rep stos/movs` with a qword main body for
  sizes ≥ `FP_MEM_QWORD_THRESHOLD` (32 bytes) and a byte path below it.
- `fp$mem_compare` uses `repe cmpsb` and exits at the first difference.
- `fp$print_hex` is always 16 digits (fixed width, diff-friendly).
- Control-flow macros emit the minimal cmp/jcc sequence — no hidden overhead.
  What you read is what executes.
- `fp$closecheck_compare` uses one `bsr` to estimate the ladder index and a
  0–2 step adjust loop (the ladder is geometric with ratio φ < 2, so a single
  bit-scan brackets the answer within a factor of 2); min/max selection is
  branch-free `cmov`.
- Mask iteration advances by `bsf` + `blsr` (two instructions per set bit);
  no per-bit branch beyond the loop test.

## Kind Machine extension contracts (Layers 1.5–5.5)

These layers follow the same contract format. The load-bearing guarantees:

### Bit math (Layer 1.5)
- `IS_POWER_OF_TWO` / `LOG2_FLOOR` / `LOG2_CEIL` / `NEXT_POWER_OF_TWO` are
  **compile-time** evaluators; they fail the build on out-of-contract input
  (e.g. `LOG2_FLOOR 0`) rather than emitting wrong code. Results are returned
  in the per-macro `…#RESULT` constant.
- `POPCOUNT` / `COUNT_LEADING_ZEROS` / `COUNT_TRAILING_ZEROS` are single
  instructions (`popcnt`/`lzcnt`/`tzcnt`), defined for **all** inputs
  including zero (unlike `bsr`/`bsf`).
- `BIT_MATH_MIN` / `BIT_MATH_MAX` take register operands only (cmov cannot
  source an immediate) and are branch-free.

### Token management (Layer 2.5)
- Token IDs are 16-bit; `0..255` are raw bytes (identity), `256..65535` are
  category tokens. Packed decode: `range_id = id shr 8`, `offset = id and 0xFF`.
- `DECLARE_TOKEN_RANGE` requires a 256-aligned, exactly-256-slot range so the
  decode is exact bit extraction.
- `TOKEN_IS_UNIQUE` proves the packed decode round-trips and that a recorded
  value has not drifted. Two names sharing one ID assemble as a deliberate
  alias; a name *redefined* to a different value fails the build.

### Lexer (Layer 2.5)
- `fp$lexer_byte` returns a token ID, or `LEXER_MORE` (-1) when the byte
  extends a pending pattern. Non-matching bytes pass through as raw tokens;
  a non-matching byte ahead of a pattern is demoted so the pattern still
  matches mid-stream (suffix demotion).
- Tables must be **prefix-free** (no pattern a prefix of another, no
  duplicates); `LEXER_TABLE_AUDIT` enforces this at assembly time.
- `fp$lexer_flush` drains pending partial patterns as raw-byte tokens, one per
  call, returning `LEXER_MORE` when empty.

### Fibonacci / CloseCheck (Layer 4.5)
- `fib_ladder` holds the exact round-to-nearest Q32.32 values of φ^-21..φ^21,
  strictly increasing (audited at assembly time), 64-byte aligned, read-only.
- `fp$closecheck_compare(S, C)` returns `0` for equal operands, `999` for a
  zero operand / sign mismatch / gap beyond the ladder top, and otherwise the
  smallest ladder rung ≥ the relative gap `(max−min)/min`.

### Sparse structures (Layer 5)
- `MASK_SET/CLEAR/TEST_BIT` are single `bts`/`btr`/`bt`; `MASK_TEST_BIT`
  exposes CF rather than a booleanized register.
- `SPARSE_ROW_APPEND` does **not** bounds-check — capacity is the caller's
  contract. Iteration order is insertion order.

### Attention scaffolding (Layer 5.5)
- `PREMIX_2x2` is alias-safe: all four products are formed in scratch before
  any output register is written. Only `imul`/`add`, no branches.
- `WEIGHT_APPLY` accumulates `value_i * V[index_i][:]` into a caller-zeroed
  output vector; inner-loop addressing is `lea`/`shl` only.
- `ATTENTION_PRUNE_POSITION` is the per-position pruning skeleton: find the
  row max (branchless `cmov`), then keep each score whose CloseCheck distance
  from the max is ≤ the threshold. Score computation (the QKᵀ GEMM) is a
  deliberate deferred seam, not implemented here.
