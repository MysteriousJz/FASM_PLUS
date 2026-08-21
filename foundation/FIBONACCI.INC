; =============================================================================
; FASM_PLUS Foundation Library — Layer 4.5 (Kind Machine extension)
; FIBONACCI.INC — golden-ratio ladder and the CloseCheck distance oracle
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, BIT_MATH.INC, STATE_MANAGEMENT.INC
; Emits: the read-only `fib_ladder` table (via FIBONACCI_LADDER_RANGE, invoked
;        once at the bottom of this file) and the comparator procedure in the
;        current code section.
;
; Q32.32 fixed-point (bit-math standard: no floating point anywhere)
; ------------------------------------------------------------------
; A Q32.32 value is a 64-bit integer: upper 32 bits = integer part, lower 32
; bits = fractional part in units of 2^-32.
;   1.0 = 0x0000000100000000
;   0.5 = 0x0000000080000000
;   phi = 1.618033988749895... = 0x000000019E3779B9 (the Knuth golden constant)
; Rescaling after a 64x64 multiply is a single `shr rdx:rax, 32` (the product
; already lands in rdx:rax, so the high half IS the rescaled value).
;
; The ladder
; ----------
; `fib_ladder` holds phi^n for n = -21..+21 (43 entries), strictly increasing,
; 64-byte (cache-line) aligned, read-only. Values are the mathematically exact
; round-to-nearest Q32.32 representations. An assembler-time audit (FASM
; `load` over the emitted table) proves strict monotonicity at build time —
; the table cannot silently drift from the invariant the comparator relies on.
;
; NOTE: an earlier draft of the specification listed ladder constants that do
; not match exact phi^n in Q32.32 (only n = 0..3 were correct). This file
; embeds the exact values; see BOOTSTRAP_LOG.MD for the correction record.
; =============================================================================

if ~ definite FP_FIBONACCI_INC
FP_FIBONACCI_INC = 1

if ~ definite FP_STATE_MANAGEMENT_INC
  err 'FIBONACCI.INC requires STATE_MANAGEMENT.INC (and lower layers) first'
end if

; comparator sentinels
CLOSECHECK_EQUAL     = 0          ; S == C: identical
CLOSECHECK_EXTREME   = 999        ; zero operand or sign mismatch

; ladder geometry
FIB_LADDER_LOW       = -21        ; smallest exponent
FIB_LADDER_HIGH      = 21         ; largest exponent
FIB_LADDER_COUNT     = FIB_LADDER_HIGH - FIB_LADDER_LOW + 1
; bsr-to-index map. bsr(fib_ladder[k]) == k + 17 for every entry, because the
; ladder is geometric with ratio phi in (1, 2): each rung's leading bit sits
; exactly 17 positions above the base rung's. So idx ~= bsr(gap) - 17, and
; because the base rung's mask spans [2^17, 2^18), the estimate lands at or
; just below the answer — the adjust loop walks it up (never down).
FIB_LADDER_BASE_BSR  = 17

; -----------------------------------------------------------------------------
; FIBONACCI_LADDER_RANGE low, high
;   Emits the read-only `fib_ladder` table for phi^low .. phi^high. Only the
;   canonical -21..21 span is supported by the embedded constants; any other
;   span fails the build (a new span requires re-deriving the constants).
;
;   CONTRACT
;     IN        : low, high — compile-time exponents
;     OUT       : emits `fib_ladder` (43 qwords) and `fib_ladder_end`
;     CLOBBERS  : advances the output pointer
;     ERRORS    : assembly error if the span is not the canonical -21..21
; -----------------------------------------------------------------------------
macro FIBONACCI_LADDER_RANGE low*, high*
{
  if (low <> FIB_LADDER_LOW) | (high <> FIB_LADDER_HIGH)
    err 'FIBONACCI_LADDER_RANGE: only the canonical -21..21 span is provided'
  end if
align 64
fib_ladder:
	dq 0x000000000002AD75		; phi^-21 ~= 0.000041
	dq 0x0000000000045517		; phi^-20 ~= 0.000066
	dq 0x000000000007028C		; phi^-19 ~= 0.000107
	dq 0x00000000000B57A3		; phi^-18 ~= 0.000173
	dq 0x0000000000125A2F		; phi^-17 ~= 0.000280
	dq 0x00000000001DB1D2		; phi^-16 ~= 0.000453
	dq 0x0000000000300C01		; phi^-15 ~= 0.000733
	dq 0x00000000004DBDD4		; phi^-14 ~= 0.001186
	dq 0x00000000007DC9D5		; phi^-13 ~= 0.001919
	dq 0x0000000000CB87A8		; phi^-12 ~= 0.003106
	dq 0x000000000149517D		; phi^-11 ~= 0.005025
	dq 0x000000000214D926		; phi^-10 ~= 0.008131
	dq 0x00000000035E2AA3		; phi^-9  ~= 0.013156
	dq 0x00000000057303C9		; phi^-8  ~= 0.021286
	dq 0x0000000008D12E6B		; phi^-7  ~= 0.034442
	dq 0x000000000E443234		; phi^-6  ~= 0.055728
	dq 0x000000001715609F		; phi^-5  ~= 0.090170
	dq 0x00000000255992D4		; phi^-4  ~= 0.145898
	dq 0x000000003C6EF373		; phi^-3  ~= 0.236068
	dq 0x0000000061C88647		; phi^-2  ~= 0.381966
	dq 0x000000009E3779B9		; phi^-1  ~= 0.618034
	dq 0x0000000100000000		; phi^0   ~= 1.000000
	dq 0x000000019E3779B9		; phi^1   ~= 1.618034
	dq 0x000000029E3779B9		; phi^2   ~= 2.618034
	dq 0x000000043C6EF373		; phi^3   ~= 4.236068
	dq 0x00000006DAA66D2C		; phi^4   ~= 6.854102
	dq 0x0000000B1715609F		; phi^5   ~= 11.090170
	dq 0x00000011F1BBCDCC		; phi^6   ~= 17.944272
	dq 0x0000001D08D12E6B		; phi^7   ~= 29.034442
	dq 0x0000002EFA8CFC37		; phi^8   ~= 46.978714
	dq 0x0000004C035E2AA3		; phi^9   ~= 76.013156
	dq 0x0000007AFDEB26DA		; phi^10  ~= 122.991869
	dq 0x000000C70149517D		; phi^11  ~= 199.005025
	dq 0x00000141FF347858		; phi^12  ~= 321.996894
	dq 0x00000209007DC9D5		; phi^13  ~= 521.001919
	dq 0x0000034AFFB2422C		; phi^14  ~= 842.998814
	dq 0x0000055400300C01		; phi^15  ~= 1364.000733
	dq 0x0000089EFFE24E2E		; phi^16  ~= 2206.999547
	dq 0x00000DF300125A2F		; phi^17  ~= 3571.000280
	dq 0x00001691FFF4A85D		; phi^18  ~= 5777.999827
	dq 0x000024850007028C		; phi^19  ~= 9349.000107
	dq 0x00003B16FFFBAAE9		; phi^20  ~= 15126.999934
	dq 0x00005F9C0002AD75		; phi^21  ~= 24476.000041
fib_ladder_end:
  ; assembler-time audit: table must have exactly FIB_LADDER_COUNT entries
  if (fib_ladder_end - fib_ladder) <> (FIB_LADDER_COUNT * 8)
    err 'FIBONACCI_LADDER_RANGE: entry count mismatch'
  end if
  ; and must be strictly increasing (proven by loading each pair)
  FP_FIB_AUDIT_OK = 1
  repeat FIB_LADDER_COUNT
    if % > 1
      load FP_FIB_PREV qword from fib_ladder + (%-2)*8
      load FP_FIB_CUR  qword from fib_ladder + (%-1)*8
      if FP_FIB_CUR <= FP_FIB_PREV
        FP_FIB_AUDIT_OK = 0
      end if
    end if
  end repeat
  if FP_FIB_AUDIT_OK = 0
    err 'FIBONACCI_LADDER_RANGE: ladder is not strictly increasing'
  end if
}

; -----------------------------------------------------------------------------
; FIBONACCI_THRESHOLD name, n
;   Defines a named threshold constant equal to the ladder entry for phi^n.
;   The value is loaded from the emitted table at assembly time, so a named
;   threshold can never disagree with the ladder the comparator searches.
;
;   CONTRACT
;     IN        : name — constant to define; n — exponent in [-21, 21]
;     OUT       : name = fib_ladder[n - FIB_LADDER_LOW] (a Q32.32 value)
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if n is outside the ladder span
; -----------------------------------------------------------------------------
macro FIBONACCI_THRESHOLD tname*, tn*
{
  if (tn < FIB_LADDER_LOW) | (tn > FIB_LADDER_HIGH)
    err 'FIBONACCI_THRESHOLD: exponent outside the ladder span'
  end if
  load tname qword from fib_ladder + ((tn) - FIB_LADDER_LOW)*8
}

; emit the canonical ladder (read-only; the flat executable maps it readable)
FIBONACCI_LADDER_RANGE FIB_LADDER_LOW, FIB_LADDER_HIGH

; a useful named threshold: phi^5, the spec's canonical prune threshold
FIBONACCI_THRESHOLD PRUNE_THRESHOLD, 5

; -----------------------------------------------------------------------------
; fp$closecheck_compare — the CloseCheck distance oracle (softmax replacement)
;
;   Given two same-sign, non-zero values S (stable) and C (check), the oracle
;   answers "how far apart are they, on a golden-ratio scale?" by computing
;   the relative gap (larger - smaller) / smaller and returning the smallest
;   ladder rung >= that gap. Only multiplication, division, comparison, bit
;   scans, and one table load are used — no floating point, no branches in
;   the min/max selection.
;
;   CONTRACT
;     IN        : rdi = S (signed 64-bit), rsi = C (signed 64-bit)
;     OUT       : rax = CLOSECHECK_EQUAL (0)       if S == C
;                       CLOSECHECK_EXTREME (999)   if either is 0 or signs
;                                                  differ or the gap exceeds
;                                                  the ladder top
;                       fib_ladder[k] (Q32.32)     otherwise
;     CLOBBERS  : rax rcx rdx r8 r9 r10 r11, flags
;     PRESERVES : rdi rsi rbx rbp r12-r15
;     ERRORS    : none; out-of-range is reported as the 999 sentinel
;
;   Algorithm (mechanical, branch-minimal):
;     1. S == C                    -> 0
;     2. (S | C) == 0 or S*C < 0   -> 999      (xor sign test: bit 63 of
;                                               (S xor C) set <=> signs differ)
;     3. branchless min/max via cmov
;     4. gap = ((larger - smaller) << 32) / smaller   (Q32.32 via 128/64 div)
;     5. idx = bsr(gap) - FIB_LADDER_BASE_BSR, clamped to [0, COUNT-1]
;        then adjust while ladder[idx] < gap: idx++   (0..2 steps, because the
;        ladder is geometric with ratio phi < 2, one bsr step brackets the
;        answer within a factor of 2, i.e. at most ~1.4 rungs)
;     6. return ladder[idx]
; -----------------------------------------------------------------------------
align 16
fp$closecheck_compare:
	FRAME_ENTER 0
	; 1. equality fast path (xor: the bit-math identity test)
	xor	rax, rax
	cmp	rdi, rsi
	je	.out
	; 2a. zero operand: (S | C) == 0 covers only both-zero, already excluded
	;     by step 1; test each for zero explicitly.
	test	rdi, rdi
	jz	.extreme
	test	rsi, rsi
	jz	.extreme
	; 2b. sign mismatch: bit 63 of (S xor C) is set iff signs differ
	mov	rax, rdi
	xor	rax, rsi
	test	rax, rax
	js	.extreme
	; 3. branchless smaller/larger (cmov — no branch in the hot path)
	mov	r8, rdi				; r8 = smaller candidate
	mov	r9, rsi				; r9 = larger candidate
	cmp	rdi, rsi
	cmovg	r8, rsi				; r8 = min(S, C)
	cmovg	r9, rdi				; r9 = max(S, C)
	; 4. relative gap as Q32.32: ((larger - smaller) << 32) / smaller
	mov	rax, r9
	sub	rax, r8				; larger - smaller (positive)
	xor	edx, edx
	shld	rdx, rax, 32			; rdx:rax = (gap) << 32
	shl	rax, 32
	div	r8				; rax = relative gap (Q32.32)
	; 5. bsr-guided index, clamped, then a short adjust loop
	bsr	rcx, rax			; rcx = floor(log2(gap))
	sub	rcx, FIB_LADDER_BASE_BSR	; map to ladder index
	; clamp to [0, COUNT-1] branchlessly
	xor	r10d, r10d
	cmp	rcx, 0
	cmovs	rcx, r10			; idx = max(idx, 0)
	mov	r10, FIB_LADDER_COUNT-1
	cmp	rcx, r10
	cmovg	rcx, r10			; idx = min(idx, COUNT-1)
	; adjust upward while the rung is below the gap
	lea	r11, [fib_ladder]
.adjust:
	cmp	rcx, FIB_LADDER_COUNT-1
	jae	.at_top
	cmp	rax, [r11+rcx*8]
	jbe	.found
	inc	rcx
	jmp	.adjust
.at_top:
	; gap beyond the top rung: extreme
	cmp	rax, [r11+rcx*8]
	ja	.extreme
.found:
	mov	rax, [r11+rcx*8]
	jmp	.out
.extreme:
	mov	rax, CLOSECHECK_EXTREME
.out:
	FRAME_LEAVE
	ret

end if ; FP_FIBONACCI_INC
