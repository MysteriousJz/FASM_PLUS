; =============================================================================
; FASM_PLUS Foundation Library — Layer 5.5 (Kind Machine extension)
; ATTENTION_SCAFFOLDING.INC — premixer and attention loop structure
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, BIT_MATH.INC, STATE_MANAGEMENT.INC,
;             SPARSE_STRUCTURES.INC, FIBONACCI.INC
; Emits: macros only; expands inline at the call site.
;
; Scope: SCAFFOLDING, not the machine
; -----------------------------------
; These macros provide the correct *structure* for the Kind Machine attention
; mechanism: the 2x2 premix, the pruning loop skeleton, and the weighted-sum
; application. Score computation (the SIMD GEMM) is a deliberate seam — the
; macro documents where it plugs in but does not implement it, so the lower
; layers can be tested independently and the hot multiply path can be swapped
; for a SIMD library without touching the loop structure.
;
; Bit-math standard: PREMIX uses imul (integer multiply — no floating point);
; index/offset arithmetic uses shifts and indexed addressing, never division.
; =============================================================================

if ~ definite FP_ATTENTION_SCAFFOLDING_INC
FP_ATTENTION_SCAFFOLDING_INC = 1

if ~ definite FP_SPARSE_STRUCTURES_INC
  err 'ATTENTION_SCAFFOLDING.INC requires SPARSE_STRUCTURES.INC first'
end if
if ~ definite FP_FIBONACCI_INC
  err 'ATTENTION_SCAFFOLDING.INC requires FIBONACCI.INC first'
end if

; -----------------------------------------------------------------------------
; PREMIX_2x2 a11, a12, a21, a22, b11, b12, b21, b22, o11, o12, o21, o22
;   2x2 integer matrix multiply:
;     o11 = a11*b11 + a12*b21      o12 = a11*b12 + a12*b22
;     o21 = a21*b11 + a22*b21      o22 = a21*b12 + a22*b22
;   All operands are 64-bit registers. The four products are accumulated into
;   four scratch registers FIRST and copied to the outputs LAST, so an output
;   register may freely alias an input or another output — no result is
;   written until every operand has been read. Generates only imul/add
;   (integer multiply per the bit-math standard) and no branches.
;
;   CONTRACT
;     IN        : a11..a22, b11..b22 — 64-bit registers (operands)
;     OUT       : o11..o22 — 64-bit registers receiving the product entries
;     CLOBBERS  : o11..o22, rax, rcx, rdx, rsi, rdi, flags
;     PRESERVES : a11..a22, b11..b22 (unless aliased with an out register)
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PREMIX_2x2 a11*, a12*, a21*, a22*, b11*, b12*, b21*, b22*, o11*, o12*, o21*, o22*
{
	; accumulate into scratch: rax=o11, rcx=o12, rdx=o21, rsi=o22
	mov	rax, a11
	imul	rax, b11			; a11*b11
	mov	rdi, a12
	imul	rdi, b21			; a12*b21
	add	rax, rdi			; o11
	mov	rcx, a11
	imul	rcx, b12			; a11*b12
	mov	rdi, a12
	imul	rdi, b22			; a12*b22
	add	rcx, rdi			; o12
	mov	rdx, a21
	imul	rdx, b11			; a21*b11
	mov	rdi, a22
	imul	rdi, b21			; a22*b21
	add	rdx, rdi			; o21
	mov	rsi, a21
	imul	rsi, b12			; a21*b12
	mov	rdi, a22
	imul	rdi, b22			; a22*b22
	add	rsi, rdi			; o22
	; write outputs last (alias-safe)
	mov	o11, rax
	mov	o12, rcx
	mov	o21, rdx
	mov	o22, rsi
}

; -----------------------------------------------------------------------------
; WEIGHT_APPLY row_reg, values_base_reg, v_ptr_reg, out_ptr_reg, d_model_reg
;   Compute out[d] = sum over the sparse row's entries of value_i * V[i][d]
;   for d in 0..d_model-1. V is a dense row-major matrix whose rows are
;   d_model qwords; the row index selects the row.
;
;   This is the gather-and-accumulate half of attention. The loop walks the
;   sparse row in insertion order; for each entry it scales the target row of
;   V and adds it into out. Inner-loop addressing uses lea/shl (index*8),
;   never division.
;
;   CONTRACT
;     IN        : row_reg — sparse row (count + indices), values_base_reg —
;                 its values array, v_ptr_reg — V matrix base, out_ptr_reg —
;                 output vector base (d_model qwords, zeroed by the caller),
;                 d_model_reg — vector length
;     OUT       : out[] += sum(value_i * V[index_i][:])
;     CLOBBERS  : rax rcx rdx r8 r9 r10 r11, flags; memory written at out[]
;     PRESERVES : row_reg values_base_reg v_ptr_reg out_ptr_reg d_model_reg
;                 and all non-volatile registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro WEIGHT_APPLY row_reg*, values_base_reg*, v_ptr_reg*, out_ptr_reg*, d_model_reg*
{
	local	.entry_loop, .dim_loop, .entry_done, .no_entries
	push	rbx
	push	r12
	push	r13
	mov	r8, [row_reg]			; entry count
	test	r8, r8
	jz	.no_entries
	xor	r9d, r9d			; entry number
.entry_loop:
	; r10 = value, r11 = &V[index][0]
	mov	r10, [values_base_reg+r9*8]
	mov	rax, [row_reg+8+r9*8]		; index
	; row offset = index * d_model * 8
	mov	r11, d_model_reg
	imul	r11, rax			; index * d_model
	shl	r11, 3				; * 8 bytes (shl, never imul 8)
	add	r11, v_ptr_reg
	; inner: for d in 0..d_model-1: out[d] += value * V[index][d]
	xor	ecx, ecx
.dim_loop:
	cmp	rcx, d_model_reg
	jae	.entry_done
	mov	rax, [r11+rcx*8]
	imul	rax, r10
	add	[out_ptr_reg+rcx*8], rax
	inc	rcx
	jmp	.dim_loop
.entry_done:
	inc	r9
	cmp	r9, r8
	jb	.entry_loop
.no_entries:
	pop	r13
	pop	r12
	pop	rbx
}

; -----------------------------------------------------------------------------
; ATTENTION_PRUNING_LOOP seq_len_reg, scores_ptr_reg, row_reg,
;                        values_base_reg, threshold_reg
;   The pruning loop skeleton: for each query position, walk that position's
;   score row, find the row maximum, and keep only the positions whose score
;   is within `threshold` of the max as judged by fp$closecheck_compare — the
;   Fibonacci distance oracle. Scores are read from a caller-supplied dense
;   matrix (row-major, seq_len qwords per row); score *computation* (QK^T)
;   is the deferred SIMD seam and is not performed here.
;
;   Per position q:
;     1. load row q of scores
;     2. rowmax = max(scores[q][:])        (linear scan, cmov-free loop)
;     3. for each key position k:
;          d = fp$closecheck_compare(rowmax, scores[q][k])
;          if d <= threshold: SPARSE_ROW_APPEND (k, scores[q][k])
;          else: pruned (skipped)
;   The result is one sparse row per query position, written sequentially;
;   the caller provides a fresh row (and its values base) per position.
;
;   This macro establishes the loop structure and calls the oracle; the
;   caller wires the per-position row pointers. It is scaffolding: a real
;   system interposes the score GEMM before step 1.
;
;   CONTRACT
;     IN        : seq_len_reg — register with sequence length
;                 scores_ptr_reg — register with score matrix base
;                 threshold_reg — register with the Q32.32 prune threshold
;     OUT       : for each q, one sparse row filled (via the caller-provided
;                 row pointers — see usage note); pruning decision per element
;     CLOBBERS  : rax rcx rdx rsi rdi r8-r11, flags; memory written to rows
;     PRESERVES : non-volatile registers
;     ERRORS    : none; rows are caller-sized (capacity is the caller's
;                 contract, as in SPARSE_ROW_APPEND)
;
;   Usage note: because each query position needs its own sparse row, the
;   macro expects the caller to supply the current row in row_reg and its
;   values base in values_base_reg, and to advance both between expansions
;   (or use the macro once per position in its own loop). The single-position
;   form is the testable unit; see SMOKE_TESTS.ASM.
; -----------------------------------------------------------------------------
macro ATTENTION_PRUNE_POSITION scores_row_reg*, seq_len_reg*, row_reg*, values_base_reg*, threshold_reg*
{
	local	.find_max, .max_done, .scan, .prune, .next, .empty
	push	rbx
	push	r12
	push	r13
	push	r14
	push	r15
	; capture the row pointers into registers that survive the oracle call
	mov	rbx, row_reg			; sparse row base
	mov	rdx, values_base_reg		; values array base
	mov	r12, scores_row_reg		; scores row base
	mov	r13, seq_len_reg		; length
	test	r13, r13
	jz	.empty
	; 1-2. row maximum (linear scan; scores are signed, cmov branchless)
	mov	r14, [r12]			; rowmax = scores[0]
	mov	rcx, 1
.find_max:
	cmp	rcx, r13
	jae	.max_done
	mov	rax, [r12+rcx*8]
	cmp	rax, r14
	cmovg	r14, rax			; branchless max
	inc	rcx
	jmp	.find_max
.max_done:
	; 3. prune walk
	xor	r15d, r15d			; k
.scan:
	cmp	r15, r13
	jae	.next
	mov	rdi, r14			; S = rowmax (stable)
	mov	rsi, [r12+r15*8]		; C = scores[k] (check)
	push	rdx				; oracle clobbers rdx
	call	fp$closecheck_compare
	pop	rdx
	; keep iff d <= threshold (d is Q32.32; threshold_reg likewise)
	cmp	rax, threshold_reg
	ja	.prune
	; keep: append (k, score) via the saved row pointers
	mov	r8, r15
	mov	r9, [r12+r15*8]
	SPARSE_ROW_APPEND rbx, rdx, r8, r9
.prune:
	inc	r15
	jmp	.scan
.next:
.empty:
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
}

end if ; FP_ATTENTION_SCAFFOLDING_INC
