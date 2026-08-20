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

end if ; FP_SMOKE_TESTS_INC
