; =============================================================================
; FASM_PLUS Foundation Library — Layer 1.5
; BIT_MATH.INC — bit-level operations as the foundational standard
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC
; Emits: no code or data by itself; all macros expand inline at the call site.
;
; Bit Math Philosophy (the Kind Machine standard)
; -----------------------------------------------
; Bit-level operations are the DEFAULT, not an optimization:
;   * no division when shifts suffice       (x / 2^n  =  x shr n)
;   * no multiplication when shifts/lea work (x * 2^n  =  x shl n)
;   * no modulo when AND works              (x mod 2^n =  x and (2^n - 1))
;   * no floating point anywhere            (fixed-point integer math only)
;   * no branches in hot paths              (cmov/setcc branchless forms)
;   * comparison via xor/test/bsr where possible
; This is mechanical sympathy: these operations execute in a single cycle
; with perfect pipelining. Every abstraction in the Kind Machine extension
; layers decomposes to these primitives.
;
; Two families live here:
;   1. Compile-time evaluators (IS_POWER_OF_TWO, NEXT_POWER_OF_TWO,
;      LOG2_FLOOR, LOG2_CEIL) — constant-expression checks and foldings that
;      fail the build loudly on bad input instead of emitting wrong code.
;   2. Runtime primitives (POPCOUNT, COUNT_LEADING_ZEROS,
;      COUNT_TRAILING_ZEROS, BIT_MATH_MIN/MAX) — single-instruction or
;      branchless sequences with explicit contracts.
; =============================================================================

if ~ definite FP_BIT_MATH_INC
FP_BIT_MATH_INC = 1

if ~ definite FP_CONTRACTS_INC
  err 'BIT_MATH.INC requires CONSTANTS.INC and CONTRACTS.INC first'
end if

; -----------------------------------------------------------------------------
; IS_POWER_OF_TWO value   (compile-time)
;   Fails assembly unless value is a non-zero power of two. Alias intent of
;   ASSERT_POWER_OF_TWO with the bit-math naming used across the extension
;   layers; the check itself is the classic (v & (v-1)) == 0 bit trick
;   evaluated by the assembler.
;
;   CONTRACT
;     IN        : value — compile-time constant
;     OUT       : none; assembly succeeds iff value = 2^n for some n >= 0
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error otherwise (0 and non-powers rejected)
; -----------------------------------------------------------------------------
macro IS_POWER_OF_TWO value*
{
  ASSERT_POWER_OF_TWO value
}

; -----------------------------------------------------------------------------
; LOG2_FLOOR value   (compile-time)
;   Folds to floor(log2(value)) for value >= 1. Iterative halving loop in the
;   assembler; value must be a compile-time constant. LOG2_FLOOR 0 is a
;   contract violation and fails the build.
;
;   CONTRACT
;     IN        : value — compile-time constant >= 1
;     OUT       : equates name LOG2_FLOOR#RESULT to floor(log2(value))
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if value < 1
; -----------------------------------------------------------------------------
macro LOG2_FLOOR value*
{
  if (value) < 1
    err 'LOG2_FLOOR: value must be >= 1'
  end if
  LOG2_FLOOR#RESULT = 0
  while (value) shr (LOG2_FLOOR#RESULT) > 1
    LOG2_FLOOR#RESULT = LOG2_FLOOR#RESULT + 1
  end while
}

; -----------------------------------------------------------------------------
; LOG2_CEIL value   (compile-time)
;   Folds to ceil(log2(value)) for value >= 1. Equivalent to LOG2_FLOOR for
;   exact powers, one more otherwise. LOG2_CEIL 0 fails the build.
;
;   CONTRACT
;     IN        : value — compile-time constant >= 1
;     OUT       : equates name LOG2_CEIL#RESULT to ceil(log2(value))
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if value < 1
; -----------------------------------------------------------------------------
macro LOG2_CEIL value*
{
  if (value) < 1
    err 'LOG2_CEIL: value must be >= 1'
  end if
  LOG2_CEIL#RESULT = 0
  while (1 shl (LOG2_CEIL#RESULT)) < (value)
    LOG2_CEIL#RESULT = LOG2_CEIL#RESULT + 1
  end while
}

; -----------------------------------------------------------------------------
; NEXT_POWER_OF_TWO value   (compile-time)
;   Folds to the smallest 2^n >= value. value must be a compile-time constant
;   >= 1. NEXT_POWER_OF_TWO 1 = 1.
;
;   CONTRACT
;     IN        : value — compile-time constant >= 1
;     OUT       : equates name NEXT_POWER_OF_TWO#RESULT to 2^n >= value
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if value < 1
; -----------------------------------------------------------------------------
macro NEXT_POWER_OF_TWO value*
{
  if (value) < 1
    err 'NEXT_POWER_OF_TWO: value must be >= 1'
  end if
  NEXT_POWER_OF_TWO#RESULT = 1
  while (NEXT_POWER_OF_TWO#RESULT) < (value)
    NEXT_POWER_OF_TWO#RESULT = (NEXT_POWER_OF_TWO#RESULT) shl 1
  end while
}

; -----------------------------------------------------------------------------
; POPCOUNT reg64   (runtime)
;   Counts set bits. Single POPCNT instruction (x86-64 feature; validated by
;   the bootstrap CPUID gate and by the smoke test against a known value).
;
;   CONTRACT
;     IN        : reg64 — value to count (read in place)
;     OUT       : reg64 = number of set bits (0..64)
;     CLOBBERS  : reg64, flags
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro POPCOUNT reg*
{
	popcnt	reg, reg
}

; -----------------------------------------------------------------------------
; COUNT_LEADING_ZEROS reg64   (runtime)
;   Number of leading zero bits (63 for value 1, 64 for value 0). Single
;   LZCNT instruction — unlike BSR it is defined for zero input, which keeps
;   callers branch-free.
;
;   CONTRACT
;     IN        : reg64 — value to inspect
;     OUT       : reg64 = count of leading zeros (0..64)
;     CLOBBERS  : reg64, flags
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro COUNT_LEADING_ZEROS reg*
{
	lzcnt	reg, reg
}

; -----------------------------------------------------------------------------
; COUNT_TRAILING_ZEROS reg64   (runtime)
;   Number of trailing zero bits (64 for value 0). Single TZCNT instruction —
;   unlike BSF it is defined for zero input.
;
;   CONTRACT
;     IN        : reg64 — value to inspect
;     OUT       : reg64 = count of trailing zeros (0..64)
;     CLOBBERS  : reg64, flags
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro COUNT_TRAILING_ZEROS reg*
{
	tzcnt	reg, reg
}

; -----------------------------------------------------------------------------
; BIT_MATH_MIN dst, a, b   (runtime, branchless)
;   dst = min(a, b) using signed comparison and CMOV — no branch, constant
;   cycle count regardless of operand values. dst may alias a.
;
;   CONTRACT
;     IN        : a, b, dst — 64-bit registers (cmov requires register
;                 sources; immediates are a contract violation)
;     OUT       : dst = signed minimum of a and b
;     CLOBBERS  : dst, flags
;     PRESERVES : a, b when distinct from dst
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro BIT_MATH_MIN dst*, a*, b*
{
	mov	dst, a
	cmp	dst, b
	cmovg	dst, b
}

; -----------------------------------------------------------------------------
; BIT_MATH_MAX dst, a, b   (runtime, branchless)
;   dst = max(a, b) using signed comparison and CMOV — no branch.
;
;   CONTRACT
;     IN        : a, b, dst — 64-bit registers (cmov requires register
;                 sources; immediates are a contract violation)
;     OUT       : dst = signed maximum of a and b
;     CLOBBERS  : dst, flags
;     PRESERVES : a, b when distinct from dst
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro BIT_MATH_MAX dst*, a*, b*
{
	mov	dst, a
	cmp	dst, b
	cmovl	dst, b
}

; -----------------------------------------------------------------------------
; Self-check: the compile-time evaluators must agree with known values at
; every inclusion, so a broken edit is caught at the first build.
; -----------------------------------------------------------------------------
IS_POWER_OF_TWO 64
LOG2_FLOOR 1
STATIC_ASSERT LOG2_FLOOR#RESULT = 0
LOG2_FLOOR 4096
STATIC_ASSERT LOG2_FLOOR#RESULT = 12
LOG2_FLOOR 4097
STATIC_ASSERT LOG2_FLOOR#RESULT = 12
LOG2_CEIL 4096
STATIC_ASSERT LOG2_CEIL#RESULT = 12
LOG2_CEIL 4097
STATIC_ASSERT LOG2_CEIL#RESULT = 13
NEXT_POWER_OF_TWO 1
STATIC_ASSERT NEXT_POWER_OF_TWO#RESULT = 1
NEXT_POWER_OF_TWO 43
STATIC_ASSERT NEXT_POWER_OF_TWO#RESULT = 64

end if ; FP_BIT_MATH_INC
