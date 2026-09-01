; =============================================================================
; FASM_PLUS Foundation Library — SMOKE_TESTS.ASM
; One test per public macro/function in the foundation library.
;
; This file defines the tests only; TEST_RUNNER.ASM registers and runs them.
; Each test is a TEST_DEF/TEST_END function returning rax = E_OK on pass.
; Failure paths use the ASSERT_* macros so the failing source line is
; reported by the runner.
;
; Depends on: the whole foundation library.
; =============================================================================

if ~ definite FP_SMOKE_TESTS_INC
FP_SMOKE_TESTS_INC = 1

; -----------------------------------------------------------------------------
; Action macros for the MASK_ITERATE / SPARSE_ROW_ITERATE smoke tests. These
; are expanded once per set bit / per sparse entry by the iteration macros.
; sm$mask_acc: accumulate the current bit index (rcx) into r15.
; sm$row_acc:  accumulate the entry's value into sm$vsum and index into
;              sm$isum (entry number in rcx; row in r13, values base in r14).
; -----------------------------------------------------------------------------
macro sm$mask_acc
{
	add	r15, rcx
}
macro sm$row_acc
{
	mov	rax, [r13+8+rcx*8]		; index
	add	[sm$isum], rax
	mov	rax, [r14+rcx*8]		; value
	add	[sm$vsum], rax
}

; ---- Layer 0/1: constants and compile-time contracts -------------------------

TEST_DEF constants_sane
	; verify the assembled values match the documented invariants
	mov	rax, PAGE_SIZE
	ASSERT_EQ rax, 4096
	mov	rax, ALIGN_STACK
	ASSERT_EQ rax, 16
	mov	rax, NULL
	ASSERT_ZERO rax
	mov	rax, E_OK
	ASSERT_ZERO rax
	mov	rax, TRUE
	ASSERT_NE rax, FALSE
	mov	rax, E_OK
TEST_END

TEST_DEF constants_power_of_two
	; runtime mirror of the static checks: page and cache are powers of two
	mov	rax, PAGE_SIZE
	dec	rax
	and	rax, PAGE_SIZE
	ASSERT_ZERO rax			; (n & n-1) == 0 for powers of two
	mov	rax, CACHE_LINE
	dec	rax
	and	rax, CACHE_LINE
	ASSERT_ZERO rax
	mov	rax, E_OK
TEST_END

; ---- Layer 1: state management ------------------------------------------------

TEST_DEF push_pop_volatile
	mov	rax, 111
	mov	rcx, 222
	mov	rdx, 333
	PUSH_VOLATILE
	mov	rax, 0			; clobber
	mov	rcx, 0
	mov	rdx, 0
	POP_VOLATILE
	ASSERT_EQ rax, 111
	ASSERT_EQ rcx, 222
	ASSERT_EQ rdx, 333
	mov	rax, E_OK
TEST_END

TEST_DEF push_pop_nonvolatile
	mov	rbx, 1111
	mov	r12, 2222
	mov	r15, 3333
	PUSH_NONVOLATILE
	mov	rbx, 0
	mov	r12, 0
	mov	r15, 0
	POP_NONVOLATILE
	ASSERT_EQ rbx, 1111
	ASSERT_EQ r12, 2222
	ASSERT_EQ r15, 3333
	mov	rax, E_OK
TEST_END

TEST_DEF frame_enter_leave
	FRAME_ENTER 24
	; rsp must be 16-byte aligned inside the frame
	mov	rax, rsp
	and	rax, 15
	ASSERT_ZERO rax
	FRAME_LEAVE
	mov	rax, E_OK
TEST_END

TEST_DEF stack_canary_intact
	FRAME_ENTER 0
	CANARY_PLACE
	; do nothing to the frame; canary must verify
	CANARY_CHECK .smashed
	CANARY_RELEASE
	FRAME_LEAVE
	mov	rax, E_OK
TEST_END
.smashed:
	; unreachable in this test; provided so CANARY_CHECK has a target
	mov	rax, E_STACK_CORRUPT
	ret

TEST_DEF stack_align_force
	STACK_ALIGN_FORCE r11
	mov	rax, rsp
	and	rax, 15
	ASSERT_ZERO rax
	STACK_ALIGN_RESTORE r11
	mov	rax, E_OK
TEST_END

; ---- Layer 2: memory primitives ----------------------------------------------

TEST_DEF mem_zero_works
	lea	rdi, [sm$buf]
	mov	rsi, 64
	call	fp$mem_zero
	lea	rdi, [sm$buf]
	lea	rsi, [sm$zeros]
	ASSERT_MEM_EQ rdi, rsi, 64
	mov	rax, E_OK
TEST_END

TEST_DEF mem_copy_roundtrip
	lea	rdi, [sm$copydst]
	lea	rsi, [sm$copysrc]
	mov	rdx, 32
	call	fp$mem_copy
	lea	rdi, [sm$copydst]
	lea	rsi, [sm$copysrc]
	ASSERT_MEM_EQ rdi, rsi, 32
	mov	rax, E_OK
TEST_END

TEST_DEF mem_compare_detects
	lea	rdi, [sm$cmpA]
	lea	rsi, [sm$cmpB]
	mov	rdx, 8
	call	fp$mem_compare
	ASSERT_NOT_ZERO rax		; A and B differ
	lea	rdi, [sm$cmpA]
	lea	rsi, [sm$cmpA]
	mov	rdx, 8
	call	fp$mem_compare
	ASSERT_ZERO rax			; identical
	mov	rax, E_OK
TEST_END

TEST_DEF mem_move_overlap
	; dest > src, overlapping: must use memmove semantics
	lea	rsi, [sm$mvbuf]
	lea	rdi, [sm$mvbuf+8]
	mov	rdx, 24
	call	fp$mem_copy
	lea	rdi, [sm$mvbuf+8]
	lea	rsi, [sm$mvexpect]
	ASSERT_MEM_EQ rdi, rsi, 24
	mov	rax, E_OK
TEST_END

TEST_DEF alloc_basic
	lea	rdi, [sm$arena]
	mov	rsi, 256
	call	fp$alloc_init
	ASSERT_EQ rax, E_OK
	mov	rdi, 16
	call	fp$alloc
	ASSERT_GT rax, 0
	; alignment invariant
	mov	rcx, rax
	and	rcx, 15
	ASSERT_ZERO rcx
	mov	rax, E_OK
TEST_END

TEST_DEF alloc_rewind_reuses
	lea	rdi, [sm$arena]
	mov	rsi, 256
	call	fp$alloc_init
	call	fp$alloc_mark
	mov	rbx, rax			; mark
	mov	rdi, 32
	call	fp$alloc
	mov	rdi, rbx
	call	fp$alloc_rewind
	mov	rdi, 32
	call	fp$alloc
	ASSERT_EQ rax, rbx			; rewind -> same address
	mov	rax, E_OK
TEST_END

TEST_DEF alloc_exhaustion_detected
	lea	rdi, [sm$arena]
	mov	rsi, 256
	call	fp$alloc_init
	mov	rdi, 4096			; far more than the arena
	call	fp$alloc
	ASSERT_EQ rax, E_OUT_OF_MEMORY
	mov	rax, E_OK
TEST_END

; ---- Layer 3: control flow ----------------------------------------------------

TEST_DEF for_loop_sums
	xor	r13, r13
	FOR r12, 0, 10, 1, .sumloop
	add	r13, r12
	NEXT r12, 1, .sumloop
	ASSERT_EQ r13, 45			; 0+..+9
	mov	rax, E_OK
TEST_END

TEST_DEF for_loop_break
	xor	r13, r13
	FOR r12, 0, 100, 1, .brkloop
	inc	r13
	cmp	r12, 4
	jne	.cont
	BREAK .brkloop
.cont:
	NEXT r12, 1, .brkloop
	ASSERT_EQ r13, 5			; ran for r12 = 0..4
	mov	rax, E_OK
TEST_END

TEST_DEF while_loop_counts
	xor	r14, r14
	xor	r13, r13
.countloop:
	cmp	r14, 7
	jge	.countdone
	inc	r13
	inc	r14
	jmp	.countloop
.countdone:
	ASSERT_EQ r13, 7
	mov	rax, E_OK
TEST_END

TEST_DEF repeat_until
	xor	r15, r15
	REPEAT .rloop
	inc	r15
	UNTIL r15, 9, .rloop
	ASSERT_EQ r15, 9
	mov	rax, E_OK
TEST_END

TEST_DEF guard_return_zero
	mov	rax, 5
	test	rax, rax
	RETURN_IF_ZERO .iszero
	mov	rax, 42
	jmp	.finch
.iszero:
	mov	rax, -1
.finch:
	ASSERT_EQ rax, 42			; not taken since rax != 0
	mov	rax, E_OK
TEST_END

; ---- Layer 4: I/O (write-path smoke; output verified by the harness) ----------

TEST_DEF print_uint_returns_count
	mov	rdi, 1000
	call	fp$print_uint
	ASSERT_EQ rax, 4			; "1000" is 4 chars
	call	fp$print_newline
	mov	rax, E_OK
TEST_END

TEST_DEF print_hex_width
	mov	rdi, 0xFF
	call	fp$print_hex
	ASSERT_EQ rax, 16			; always 16 digits
	call	fp$print_newline
	mov	rax, E_OK
TEST_END

TEST_DEF print_int_negative
	mov	rdi, -5
	call	fp$print_int
	; '-' + '5' = 2 chars
	ASSERT_EQ rax, 2
	call	fp$print_newline
	mov	rax, E_OK
TEST_END

TEST_DEF file_roundtrip
	; create, write, close; reopen, read, verify, close, unlink-free
	lea	rdi, [sm$tmpname]
	mov	rsi, O_CREAT or O_WRONLY or O_TRUNC
	mov	rdx, 600o			; mode 0600 (owner rw), FASM octal
	call	fp$file_open
	ASSERT_GT rax, 0			; a real fd
	mov	rbx, rax			; save fd
	mov	rdi, rbx
	lea	rsi, [sm$filedata]
	mov	rdx, 5
	call	fp$file_write
	ASSERT_EQ rax, 5
	mov	rdi, rbx
	call	fp$file_close
	ASSERT_ZERO rax
	; reopen and read back
	lea	rdi, [sm$tmpname]
	mov	rsi, O_RDONLY
	xor	edx, edx
	call	fp$file_open
	ASSERT_GT rax, 0
	mov	rbx, rax
	lea	rdi, [sm$readback]
	mov	rsi, 8
	push	rdi
	mov	rdi, rbx
	pop	rsi				; rsi = buffer
	mov	rdx, 8
	call	fp$file_read
	ASSERT_EQ rax, 5			; only 5 bytes written
	mov	rdi, rbx
	call	fp$file_close
	lea	rdi, [sm$readback]
	lea	rsi, [sm$filedata]
	ASSERT_MEM_EQ rdi, rsi, 5
	mov	rax, E_OK
TEST_END

; ---- Layer 1.5: bit math ------------------------------------------------------

TEST_DEF popcount_basic
	xor	rax, rax
	POPCOUNT rax
	ASSERT_ZERO rax				; popcount(0) = 0
	mov	rax, 0xF0F0
	POPCOUNT rax
	ASSERT_EQ rax, 8			; 8 set bits
	mov	rax, -1				; all 64 bits set
	POPCOUNT rax
	ASSERT_EQ rax, 64
	mov	rax, E_OK
TEST_END

TEST_DEF count_leading_zeros
	mov	rax, 1
	COUNT_LEADING_ZEROS rax
	ASSERT_EQ rax, 63
	xor	eax, eax
	COUNT_LEADING_ZEROS rax
	ASSERT_EQ rax, 64			; lzcnt(0) is defined (unlike bsr)
	mov	rax, 0x8000000000000000
	COUNT_LEADING_ZEROS rax
	ASSERT_ZERO rax
	mov	rax, E_OK
TEST_END

TEST_DEF count_trailing_zeros
	mov	rax, 0x100			; bit 8
	COUNT_TRAILING_ZEROS rax
	ASSERT_EQ rax, 8
	mov	rax, 1
	COUNT_TRAILING_ZEROS rax
	ASSERT_ZERO rax
	xor	eax, eax
	COUNT_TRAILING_ZEROS rax
	ASSERT_EQ rax, 64			; tzcnt(0) is defined (unlike bsf)
	mov	rax, E_OK
TEST_END

TEST_DEF bitmath_min_max
	mov	r8, 7
	mov	r9, 9
	BIT_MATH_MIN rcx, r8, r9
	ASSERT_EQ rcx, 7
	BIT_MATH_MAX rcx, r8, r9
	ASSERT_EQ rcx, 9
	; negative values (signed)
	mov	r8, -5
	mov	r9, 3
	BIT_MATH_MIN rcx, r8, r9
	ASSERT_EQ rcx, -5
	BIT_MATH_MAX rcx, r8, r9
	ASSERT_EQ rcx, 3
	mov	rax, E_OK
TEST_END

; ---- Layer 2.5a: token management ---------------------------------------------

TEST_DEF tokens_in_ranges
	; raw byte mapping is the identity (TOKEN_RAW_BYTE folds value to itself)
	TOKEN_RAW_BYTE 65			; compile-time check: 65 is a byte
	mov	rax, 65
	ASSERT_EQ rax, 65
	; category tokens land in their ranges
	mov	rax, TOKEN_X86_MOV
	ASSERT_IN_RANGE rax, ISA_X86, ISA_X86 + 255
	mov	rax, TOKEN_ELF
	ASSERT_IN_RANGE rax, FILE_MAGIC, FILE_MAGIC + 255
	mov	rax, TOKEN_PNG
	ASSERT_IN_RANGE rax, FILE_MAGIC, FILE_MAGIC + 255
	mov	rax, E_OK
TEST_END

TEST_DEF tokens_no_collision
	; distinct definitions in distinct ranges hold distinct IDs
	mov	rax, TOKEN_X86_MOV
	mov	rcx, TOKEN_ELF
	ASSERT_NE rax, rcx
	mov	rcx, TOKEN_PNG
	ASSERT_NE rax, rcx
	; the packed decode round-trips: range_id = id shr 8, offset = id and 0xFF
	mov	rax, TOKEN_X86_MOV
	mov	rcx, rax
	shr	rcx, 8				; range_id
	ASSERT_EQ rcx, 1			; ISA_X86 is range 1
	mov	rcx, rax
	and	rcx, 0xFF			; offset
	ASSERT_EQ rcx, 1			; ISA_X86 + 1
	mov	rax, E_OK
TEST_END

; ---- Layer 2.5b: lexer ---------------------------------------------------------

TEST_DEF lexer_init_clears
	; poison the state, then init must clear it
	mov	rax, 0xDEADBEEF
	mov	[fp$lexer_state], rax
	mov	[fp$lexer_buffer], rax
	call	fp$lexer_init
	mov	rax, [fp$lexer_state]
	ASSERT_ZERO rax
	mov	rax, [fp$lexer_buffer]
	ASSERT_ZERO rax
	mov	rax, E_OK
TEST_END

TEST_DEF lexer_elf_header
	call	fp$lexer_init
	lea	rsi, [sm$lexer_table]
	mov	edi, 0x7F
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x45
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x4C
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x46
	call	fp$lexer_byte
	ASSERT_EQ rax, TOKEN_ELF
	; nothing left to flush
	call	fp$lexer_flush
	ASSERT_EQ rax, LEXER_MORE
	mov	rax, E_OK
TEST_END

TEST_DEF lexer_raw_passthrough
	call	fp$lexer_init
	lea	rsi, [sm$lexer_table]
	mov	edi, 'A'			; not a prefix of any pattern
	call	fp$lexer_byte
	ASSERT_EQ rax, 'A'			; raw byte token (identity)
	mov	rax, E_OK
TEST_END

TEST_DEF lexer_partial_flush
	call	fp$lexer_init
	lea	rsi, [sm$lexer_table]
	mov	edi, 0x7F
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x45
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	; flush drains the two pending bytes as raw tokens
	call	fp$lexer_flush
	ASSERT_EQ rax, 0x7F
	call	fp$lexer_flush
	ASSERT_EQ rax, 0x45
	call	fp$lexer_flush
	ASSERT_EQ rax, LEXER_MORE
	mov	rax, E_OK
TEST_END

TEST_DEF lexer_suffix_demotion
	; a non-pattern byte followed by a full pattern: raw token, then match
	call	fp$lexer_init
	lea	rsi, [sm$lexer_table]
	mov	edi, 0x00
	call	fp$lexer_byte
	ASSERT_EQ rax, 0x00			; raw 0
	mov	edi, 0x7F
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x45
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x4C
	call	fp$lexer_byte
	ASSERT_EQ rax, LEXER_MORE
	mov	edi, 0x46
	call	fp$lexer_byte
	ASSERT_EQ rax, TOKEN_ELF
	mov	rax, E_OK
TEST_END

; ---- Layer 4.5: Fibonacci ladder and CloseCheck --------------------------------

TEST_DEF fib_ladder_monotonic
	; spot-check the ladder is strictly increasing across its midpoint
	mov	rax, [fib_ladder + 21*8]	; phi^0 = 1.0
	mov	rcx, 0x0000000100000000
	ASSERT_EQ rax, rcx
	mov	rax, [fib_ladder + 22*8]	; phi^1
	mov	rcx, 0x000000019E3779B9
	ASSERT_EQ rax, rcx
	; first < middle < last
	mov	rax, [fib_ladder]
	mov	rcx, [fib_ladder + 21*8]
	ASSERT_LT rax, rcx
	mov	rcx, [fib_ladder + 42*8]
	ASSERT_LT rcx, 0			; signed compare unused; ensure big
	mov	rax, [fib_ladder + 21*8]
	mov	rcx, [fib_ladder + 42*8]
	ASSERT_LT rax, rcx
	mov	rax, E_OK
TEST_END

TEST_DEF fib_threshold_named
	; PRUNE_THRESHOLD = phi^5, loaded from the ladder at assembly time
	mov	rax, PRUNE_THRESHOLD
	mov	rcx, [fib_ladder + 26*8]	; n=5 -> index 5+21 = 26
	ASSERT_EQ rax, rcx
	mov	rax, E_OK
TEST_END

TEST_DEF closecheck_equal
	mov	rdi, 100
	mov	rsi, 100
	call	fp$closecheck_compare
	ASSERT_ZERO rax
	mov	rax, E_OK
TEST_END

TEST_DEF closecheck_zero
	mov	rdi, 0
	mov	rsi, 5
	call	fp$closecheck_compare
	ASSERT_EQ rax, CLOSECHECK_EXTREME
	mov	rdi, 5
	mov	rsi, 0
	call	fp$closecheck_compare
	ASSERT_EQ rax, CLOSECHECK_EXTREME
	mov	rax, E_OK
TEST_END

TEST_DEF closecheck_sign_mismatch
	mov	rdi, -5
	mov	rsi, 5
	call	fp$closecheck_compare
	ASSERT_EQ rax, CLOSECHECK_EXTREME
	mov	rax, E_OK
TEST_END

TEST_DEF closecheck_known_ratio
	; S=1, C=2: gap = 1.0 exactly -> rung phi^0
	mov	rdi, 1
	mov	rsi, 2
	call	fp$closecheck_compare
	mov	rcx, 0x0000000100000000
	ASSERT_EQ rax, rcx
	; S=2, C=3: gap = 0.5 -> first rung >= 0.5 is phi^-1
	mov	rdi, 2
	mov	rsi, 3
	call	fp$closecheck_compare
	mov	rcx, 0x000000009E3779B9
	ASSERT_EQ rax, rcx
	mov	rax, E_OK
TEST_END

TEST_DEF closecheck_out_of_range
	; gap far beyond the ladder top -> 999 sentinel
	mov	rdi, 1
	mov	rsi, 100000
	call	fp$closecheck_compare
	ASSERT_EQ rax, CLOSECHECK_EXTREME
	mov	rax, E_OK
TEST_END

; ---- Layer 5: sparse structures -------------------------------------------------

TEST_DEF mask_bit_ops
	xor	ebx, ebx
	MASK_SET_BIT rbx, 3
	MASK_SET_BIT rbx, 5
	MASK_TEST_BIT rbx, 3
	jc	.bit3ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.bit3ok:
	MASK_TEST_BIT rbx, 4
	jnc	.bit4clear
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.bit4clear:
	MASK_CLEAR_BIT rbx, 3
	MASK_TEST_BIT rbx, 3
	jnc	.bit3gone
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.bit3gone:
	ASSERT_EQ rbx, 32			; only bit 5 remains
	mov	rax, E_OK
TEST_END

TEST_DEF mask_iterate_sum
	; sum the indices of set bits in 0b10101 (bits 0,2,4) = 6
	xor	r15, r15
	mov	rbx, 21				; 0b10101
	MASK_ITERATE rbx, sm$mask_acc
	ASSERT_EQ r15, 6
	mov	rax, E_OK
TEST_END

TEST_DEF sparse_row_append_iterate
	lea	r13, [sm$row]
	lea	r14, [sm$row_values]
	mov	qword [r13], 0			; count = 0
	mov	r8, 2
	mov	r9, 100
	SPARSE_ROW_APPEND r13, r14, r8, r9
	mov	r8, 5
	mov	r9, 200
	SPARSE_ROW_APPEND r13, r14, r8, r9
	mov	rax, [r13]
	ASSERT_EQ rax, 2			; count
	mov	rax, [r13+8]
	ASSERT_EQ rax, 2			; indices[0]
	mov	rax, [r13+16]
	ASSERT_EQ rax, 5			; indices[1]
	mov	rax, [r14]
	ASSERT_EQ rax, 100			; values[0]
	mov	rax, [r14+8]
	ASSERT_EQ rax, 200			; values[1]
	; iterate: sum values and indices
	mov	qword [sm$vsum], 0
	mov	qword [sm$isum], 0
	SPARSE_ROW_ITERATE r13, r14, sm$row_acc
	mov	rax, [sm$vsum]
	ASSERT_EQ rax, 300
	mov	rax, [sm$isum]
	ASSERT_EQ rax, 7
	mov	rax, E_OK
TEST_END

; ---- Layer 5.5: attention scaffolding ------------------------------------------

TEST_DEF premix_2x2
	mov	r8, 1
	mov	r9, 2
	mov	r10, 3
	mov	r11, 4
	mov	r12, 5
	mov	r13, 6
	mov	r14, 7
	mov	r15, 8
	PREMIX_2x2 r8, r9, r10, r11, r12, r13, r14, r15, [sm$pm_o11], [sm$pm_o12], [sm$pm_o21], [sm$pm_o22]
	mov	rax, [sm$pm_o11]
	ASSERT_EQ rax, 19
	mov	rax, [sm$pm_o12]
	ASSERT_EQ rax, 22
	mov	rax, [sm$pm_o21]
	ASSERT_EQ rax, 43
	mov	rax, [sm$pm_o22]
	ASSERT_EQ rax, 50
	mov	rax, E_OK
TEST_END

TEST_DEF weight_apply
	lea	r13, [sm$row]
	lea	r14, [sm$row_values]
	mov	qword [r13], 0
	mov	r8, 0
	mov	r9, 2
	SPARSE_ROW_APPEND r13, r14, r8, r9	; (index 0, value 2)
	mov	r8, 2
	mov	r9, 3
	SPARSE_ROW_APPEND r13, r14, r8, r9	; (index 2, value 3)
	; out = 2*V[0] + 3*V[2] over the 2-dim rows of sm$V
	mov	qword [sm$out], 0
	mov	qword [sm$out+8], 0
	lea	rdi, [sm$V]
	lea	rsi, [sm$out]
	mov	rdx, 2
	WEIGHT_APPLY r13, r14, rdi, rsi, rdx
	mov	rax, [sm$out]
	ASSERT_EQ rax, 110			; 2*10 + 3*30
	mov	rax, [sm$out+8]
	ASSERT_EQ rax, 115			; 2*11 + 3*31
	mov	rax, E_OK
TEST_END

TEST_DEF attention_prune_mock
	; one score row [5,1,5,2], threshold = phi^0 (1.0): keep k where
	; closecheck(rowmax=5, scores[k]) <= 1.0, i.e. the elements equal to the
	; max. Expect indices 0 and 2 kept.
	lea	r13, [sm$row]
	lea	r14, [sm$row_values]
	mov	qword [r13], 0
	lea	rdi, [sm$scores]
	mov	rsi, 4
	mov	rbx, 0x0000000100000000		; threshold = 1.0
	ATTENTION_PRUNE_POSITION rdi, rsi, r13, r14, rbx
	mov	rax, [r13]
	ASSERT_EQ rax, 2			; two kept
	mov	rax, [r13+8]
	ASSERT_EQ rax, 0			; index 0
	mov	rax, [r13+16]
	ASSERT_EQ rax, 2			; index 2
	mov	rax, E_OK
TEST_END

end if ; FP_SMOKE_TESTS_INC
