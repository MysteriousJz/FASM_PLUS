# Bootstrap Log

A record of how the foundation was built, the decisions made, the surprises
encountered, and the work left for later. Written as the implementation
proceeded, bottom-up, so it doubles as a learning artifact.

## Process

The layers were implemented strictly bottom-up, each assembled and tested
before the next began:

1. `CONSTANTS.INC`
2. `CONTRACTS.INC`
3. `STATE_MANAGEMENT.INC`
4. `MEMORY_PRIMITIVES.INC`
5. `CONTROL_FLOW.INC`
6. `IO_BASIC.INC`
7. `TEST_FRAMEWORK.INC`
8. `BOOTSTRAP.INC`
9. `CRASH_DETECTOR.INC`
10. `SMOKE_TESTS.ASM` + `SMOKE_DATA.INC` + `TEST_RUNNER.ASM`
11. `EXAMPLE.ASM` + `build.sh`
12. Documentation

Every layer had a throwaway program in `/tmp` that exercised it before the
layer was considered done. The final suite (`TEST_RUNNER.ASM`) consolidates
those checks into 47 registered tests (23 foundation + 24 Kind Machine
extension).

## Decisions and the reasoning behind them

### Include guards use `definite`, not `defined`
FASM's `defined` operator permits forward references, so `if ~defined GUARD`
evaluates differently across assembly passes and breaks FASM's output tracking
with `error: code cannot be generated`. `definite` checks only symbols defined
*earlier* in the source, giving deterministic single-evaluation guard
semantics. This was the first real FASM subtlety encountered and is now
enforced by the guard idiom in every file.

### `format` must be the first directive
Test harnesses declare `format elf64 executable` and `entry` *before*
including the library, because FASM requires `format` first. The library files
therefore emit no `format`/`entry` and are safe to include after them.

### No `[rel]` addressing in flat ELF64 executables
`[rel symbol]` is unsupported by FASM 1.73 in `format elf64 executable` mode
(it is an object-format/PE feature). The library uses plain absolute
addressing (`[symbol]`), which the flat executable resolves at assembly time.
**Consequence:** the flat-executable build is position-*dependent*. See
"Known limitations."

### Data is separated into one closer file
Following the HeavyThing convention (`ht_data.inc`), no code include switches
segments on the caller. All writeable state lives in `FOUNDATION_DATA.INC`,
included exactly once, last. Smoke fixtures live in `SMOKE_DATA.INC`. This
keeps include order predictable and avoids the flat-format's restriction on
re-opening named sections.

### Explicit test registration over auto-discovery
FASM flat executables cannot append to a table across separately-included
files, and even where possible, implicit discovery hides ordering. Tests are
registered explicitly with `REGISTER_TEST` in `TEST_RUNNER.ASM`. This is
greppable, order-deterministic, and impossible to silently mis-wire — a
deliberate trade of convenience for trust.

### Fixed stack canary value
`STACK_CANARY_VALUE` is a fixed high-entropy constant, not a per-run random
value. A random canary would be stronger against a determined attacker, but
the foundation's goal is *deterministic detection of linear stack smashes
during development*, not adversarial hardening. Determinism keeps tests
reproducible. Runtime-random canaries are listed as future work.

### Crash handler exits with `EXIT_CRASH` instead of re-raising
After capturing RIP, fault address, signal number, and the register snapshot,
the handler writes a one-line report to stderr and `exit_group(EXIT_CRASH)`.
Re-raising the signal to die with the original status (e.g. 139) requires
unblocking the signal and a self-`kill` mid-handler — fragile in async-signal
context. A dedicated exit code is a deterministic, observable "we crashed and
captured state" that the runner checks. Captured state stays in `fp$crash_*`
for any harness that forks the process.

### Only async-signal-safe calls inside the crash handler
The handler calls nothing but raw `write(2)` via `syscall`. No allocation, no
locks, no buffered I/O — so it is safe to run on an alternate or corrupted
stack.

## Bugs found during bootstrap

These were caught by the verification discipline, not by luck:

1. **`fp$init` stack misalignment crash.** The first version aligned `rsp`
   *after* the `call` had pushed the return address, then `ret` read the wrong
   stack slot — a guaranteed crash. Fixed by establishing a `rbp` frame first
   and restoring `rsp` from `rbp` before `ret`. This is precisely the class of
   bug the "controlled halt / no silent corruption" rule targets; it surfaced
   immediately because the example exercised `fp$init` before anything else.

2. **`fp$mem_compare` sign inversion.** `cmpsb` computes `[rsi] - [rdi]`,
   opposite to the A-vs-B contract. The initial flag-based sign was backwards.
   Fixed by reloading the two differing bytes and subtracting explicitly, so
   the sign is unambiguous.

3. **Test-oracle bug (overlap move).** The overlap test's expected value was
   wrong, not the library: after a forward overlapping memmove, `buf[0:n]` and
   `buf[8:8+n]` legitimately differ. The library was correct; the oracle was
   fixed to compare against the original source bytes. A reminder that tests
   need verification too.

## Assumptions

- Target is x86-64 Linux; the kernel ABI (syscall numbers, signal numbers,
  ucontext layout) is stable and as documented.
- x86-64 mandates CPUID, SSE, and SSE2, so the bootstrap's CPU check is a
  sanity gate, not real discovery.
- The ucontext greg offsets in `CRASH_DETECTOR.INC` match the x86-64 Linux
  `REG_*` ordering (verified against a live SIGSEGV during testing).
- Flat-executable consumers declare `format`/`entry` before including.

## Known limitations

- **Position-dependent output.** The flat `elf64 executable` build uses
  absolute addressing. True position independence is achievable by switching
  to `format elf64` object output + a link step and RIP-relative addressing;
  that path is not wired up in this foundation.
- **Single-threaded.** Global state (allocator pointer, test counters, crash
  snapshot) is unsynchronized.
- **Bump allocator has no free.** By design; use mark/rewind. A general
  allocator is higher-layer work.
- **I/O is unbuffered and blocking.** Fine for tests and tools; not a
  high-throughput I/O layer.
- **No test timeout mechanism.** The runner relies on tests terminating; a
  hanging test hangs the runner. A watchdog (SIGALRM-based) is future work.
- **`fp$guard_map` failure cleanup.** On `mprotect` failure it unmaps but
  returns a generic `E_IO` rather than the specific errno.

## Future work (not in this foundation)

- Data structures layer (arrays, lists, queues) on top of the memory layer.
- Concurrency primitives and per-thread allocators.
- Networking (epoll, sockets) following the HeavyThing patterns.
- Persistence and security layers.
- Runtime-random stack canaries.
- Test timeouts and richer post-mortem (stack unwinding, symbolized RIP).
- Position-independent object build and FASMG variants.

## Verification status at handoff

- `foundation/build.sh test` → `tests: 47 failed: 0`, exit code 0.
- Failure injection (deliberately broken assertion) → reported with the
  correct name and line, exit code 2.
- Deliberate NULL dereference → `CRASH sig=11 rip=0x.. addr=0x0` on stderr,
  exit code 3, register snapshot captured.
- `EXAMPLE.ASM` runs cleanly, exercising bootstrap, CPUID, the allocator,
  structured loops, formatted I/O, and guard-page mapping.

---

## Kind Machine extension (Layers 1.5, 2.5, 4.5, 5, 5.5)

A second build phase added the mechanical primitives for a hardware-kind
attention mechanism: bit-math primitives, a token vocabulary, a pattern lexer,
the golden-ratio distance oracle, sparse structures, and attention scaffolding.
The same bottom-up discipline applied — each layer was exercised by a
throwaway program in `/tmp` before being wired into the smoke suite. 24 tests
were added (23 → 47).

### Bit math is the mandated standard

Per the extension requirement, bit-level operations are the default, not an
optimization: shifts over division, AND over modulo, `cmov`/`setcc` over
branches in hot paths, and no floating point anywhere (the ladder is Q32.32).
`BIT_MATH.INC` codifies this with single-instruction wrappers (`popcnt`,
`lzcnt`, `tzcnt`) chosen specifically because they are defined for zero input
— unlike `bsr`/`bsf` — which keeps callers branch-free.

### The spec's Fibonacci ladder constants were wrong — exact values embedded

The draft specification listed 43 Q32.32 constants for φ^-21..φ^21. Only the
four for n = 0..3 match exact φ^n in Q32.32; the other 39 are incorrect (for
example it gave φ^-1 = 0x5BCAA, where the exact Q32.32 value is 0x9E3779B9 —
the well-known Knuth golden-ratio fixed-point constant). Rather than propagate
the error, `FIBONACCI.INC` embeds the mathematically exact round-to-nearest
values. The build-time monotonic audit (below) would have caught any residual
drift, and the comparator tests use hand-computed expected rungs. This is the
verification discipline working as intended: a spec table is an oracle, and
oracles need checking too.

### Assembler-time audits via `load`/`repeat`

Two invariants are proven at assembly time by reading the emitted binary back
with FASM's `load`: the lexer table's prefix-freeness (`LEXER_TABLE_AUDIT`,
O(n²) pairwise) and the ladder's strict monotonicity. A malformed table fails
the build naming the macro — the table cannot silently drift from the
invariant its consumer relies on. This extends the foundation's STATIC_ASSERT
philosophy from scalar constants to structured data.

### FASM subtleties encountered (and worked around)

- **`= `<>` bind tighter than `and` in `if`.** `(x and 0x8000) = 0` parses as
  `x and (0x8000 = 0)`. The masked value must be materialized into an
  intermediate `=`-constant before comparison.
- **`&` in `if` does not short-circuit like a logical AND.** Compound
  conditions are expressed with nested `if` instead of `A & B`.
- **`rept` is preprocessor-time** and cannot see `=`-defined constants; loop
  counts for it must come from `define`/`equ`. The assembler's `repeat`/`%`
  (which *can* see assembler values) is used for the data audits.
- **`while` does not break on an assignment inside its body.** A "break"
  requires a done-flag incorporated into the loop condition.
- **`cmov` cannot source an immediate**, so `BIT_MATH_MIN/MAX` take register
  operands only (documented in their contracts).
- **Binary literals `0b...`** collided with FASM's tokenizer in one context;
  decimal/hex used instead.

### Bugs found by the extension's own tests

1. **Lexer MASKQ bit-vs-byte count.** The buffer mask was computed as
   `(1<<pos)-1` (bits) where `(1<<(pos*8))-1` (bytes) was required; the first
   partial-pattern byte fell through to raw passthrough. Caught by the ELF
   prefix test returning 127 instead of `LEXER_MORE`.
2. **PREMIX_2x2 output aliasing.** Storing each output register as soon as its
   product was formed clobbered a later term's operand when an output aliased
   an input. Fixed by accumulating all four products in scratch registers and
   writing outputs last — the macro is now alias-safe by construction.
3. **ATTENTION_PRUNE_POSITION register capture.** The macro shadowed the
   caller's row pointers across the `fp$closecheck_compare` call; fixed by
   capturing them into preserved registers (`rbx`, saved `rdx`) before the
   loop. Caught by a segfault in the mock-attention test.
4. **Pre-existing `ASSERT_IN_RANGE` label collision.** Its `.fail` label was
   not `local`, so a second use in one test redefined the symbol. Fixed to
   `local .fail` — required by the multi-range token test.

### Deferred by design (scaffolding seams)

- `ATTENTION_PRUNE_POSITION` reads a caller-supplied score row; the QKᵀ GEMM
  that would *produce* those scores is a deliberate seam for a future SIMD
  library, so the loop structure is testable now and the multiply path can be
  swapped without touching it.
- The comparator uses one `div` for the relative gap (correctness first). A
  reciprocal-multiply (Newton-Raphson) refinement is the documented
  optimization path; the `bsr`-guided index already removes the log-search.
- Masks wider than 64 bits are handled as qword arrays at the call site
  (index `shr 6` for the word, `and 63` for the bit); a dedicated wide-mask
  macro set is future work.
