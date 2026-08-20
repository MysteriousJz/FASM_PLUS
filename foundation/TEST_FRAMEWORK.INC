; =============================================================================
; FASM_PLUS Foundation Library — Layer 6
; TEST_FRAMEWORK.INC — assertions, test definition, and the run-all driver
;
; Depends on: all lower layers (CONSTANTS .. IO_BASIC)
; Emits: functions in the current code section; all strings and the registry
;        table live in FOUNDATION_DATA.INC / the test registry region (see
;        REGISTER_TEST below and TEST_RUNNER.ASM).
;
; Registration model
; ------------------
; Tests are functions `test$<name>` returning rax = E_OK on pass or non-zero
; on fail. The runner lists each test explicitly with REGISTER_TEST between
; fp$test_registry_begin/end. Explicit registration (rather than automatic
; discovery) is a deliberate choice: it is greppable, order-deterministic,
; and impossible to silently mis-wire. FP_TEST_TOTAL is computed by the
; runner from the table extent.
;
; Assertion model
; ---------------
; On failure an assertion stores the assemble-time source line (__line__) in
; fp$assert_line, sets rax = E_ASSERTION_FAILED, and jumps to the test's
; .epilogue, so a failing assertion unwinds cleanly instead of crashing.
; =============================================================================

if ~ definite FP_TEST_FRAMEWORK_INC
FP_TEST_FRAMEWORK_INC = 1

if ~ definite FP_IO_BASIC_INC
  err 'TEST_FRAMEWORK.INC requires IO_BASIC.INC (and all lower layers) first'
end if

; registry entries are 16 bytes: { name cstr ptr, function ptr }
FP_TEST_ENTRY_SIZE = 16

; -----------------------------------------------------------------------------
; REGISTER_TEST name_label, function_label
;   Append one 16-byte entry. Used only inside the registry table region in
;   the runner source.
; -----------------------------------------------------------------------------
macro REGISTER_TEST name_lbl*, fn_lbl*
{
	dq name_lbl
	dq fn_lbl
}

; -----------------------------------------------------------------------------
; Assertion macros. On failure: record line, set rax, jump to .epilogue.
;
;   CONTRACT (all ASSERT_*)
;     IN        : operands per macro; runs inside a TEST_DEF body
;     OUT       : falls through on success; on failure records the line in
;                 fp$assert_line and jumps to .epilogue with rax set
;     CLOBBERS  : rax, flags, memory at fp$assert_line
;     ERRORS    : none returned; failure is a branch to the epilogue
; -----------------------------------------------------------------------------
macro ASSERT_EQ reg*, expected*
{
	local	.ok
	cmp	reg, expected
	je	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_NE reg*, notexpected*
{
	local	.ok
	cmp	reg, notexpected
	jne	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_GT reg*, bound*
{
	local	.ok
	cmp	reg, bound
	jg	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_LT reg*, bound*
{
	local	.ok
	cmp	reg, bound
	jl	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_IN_RANGE reg*, low*, high*
{
	local	.ok
	cmp	reg, low
	jl	.fail
	cmp	reg, high
	jg	.fail
	jmp	.ok
.fail:
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_ZERO reg*
{
	local	.ok
	test	reg, reg
	jz	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}
macro ASSERT_NOT_ZERO reg*
{
	local	.ok
	test	reg, reg
	jnz	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}

; ASSERT_MEM_EQ areg, breg, count
;   Runtime memory-equality assertion via fp$mem_compare.
;   CONTRACT: IN areg/breg = pointers, count = bytes; falls through if the
;   regions are byte-identical. Preserves the argument registers (they are
;   saved around the call) so the test body can keep using them.
macro ASSERT_MEM_EQ areg*, breg*, count*
{
	local	.ok
	push	rdi
	push	rsi
	push	rdx
	mov	rdi, areg
	mov	rsi, breg
	mov	rdx, count
	call	fp$mem_compare
	test	rax, rax
	pop	rdx
	pop	rsi
	pop	rdi
	jz	.ok
	mov	rax, E_ASSERTION_FAILED
	mov	qword [fp$assert_line], __line__
	jmp	.epilogue
.ok:
}

; -----------------------------------------------------------------------------
; TEST_DEF name  ... body ...  TEST_END
;   Define a test function test$<name> with an .epilogue for assertions.
;
;   CONTRACT TEST_DEF
;     IN        : name = label suffix; defines callable test$<name>
;     OUT       : a test whose assertions unwind to .epilogue
;   CONTRACT TEST_END
;     IN        : rax = E_OK (pass) or a failure code
;     OUT       : returns rax to the runner
; -----------------------------------------------------------------------------
macro TEST_DEF tname*
{
align 16
test$#tname:
}
macro TEST_END
{
.epilogue:
	ret
}

; -----------------------------------------------------------------------------
; fp$test_run_all — execute every registered test and report results
;   CONTRACT
;     IN        : none; relies on FP_TEST_TOTAL and fp$test_registry_begin
;                 supplied by the runner's data section
;     OUT       : rax = number of failed tests (0 = all passed)
;     CLOBBERS  : rax rbx rbp r12-r15 rdi rsi and volatile registers; this is
;                 a top-level driver, not for use inside other tests
;     ERRORS    : none returned; each test's result is reported, and a test
;                 that faults is caught by the crash-detector layer
;   For each failure it prints "FAIL <name> line <n>"; after all tests it
;   prints "tests: <total> failed: <n>". State (non-volatile registers) is
;   saved around each call so one test cannot corrupt the next.
; =============================================================================
align 16
fp$test_run_all:
	PUSH_NONVOLATILE
	FRAME_ENTER 8
	xor	r12, r12			; index
	xor	r13, r13			; failure count
	mov	r14, fp$test_registry_begin
.loop:
	cmp	r12, FP_TEST_TOTAL
	jae	.summary
	mov	[fp$test_current], r12
	mov	qword [fp$assert_line], 0
	mov	r15, [r14+8]			; function pointer
	call	r15
	test	rax, rax
	jz	.passed
	; failure: print "FAIL <name> line <n>\n"
	inc	r13
	lea	rsi, [fp$str_fail]
	call	fp$print_cstr
	mov	rsi, [r14]			; name cstr
	call	fp$print_cstr
	lea	rsi, [fp$str_line]
	call	fp$print_cstr
	mov	rdi, [fp$assert_line]
	call	fp$print_uint
	call	fp$print_newline
.passed:
	add	r14, FP_TEST_ENTRY_SIZE
	inc	r12
	jmp	.loop
.summary:
	lea	rsi, [fp$str_tests]
	call	fp$print_cstr
	mov	rdi, FP_TEST_TOTAL
	call	fp$print_uint
	lea	rsi, [fp$str_failed]
	call	fp$print_cstr
	mov	rdi, r13
	call	fp$print_uint
	call	fp$print_newline
	mov	rax, r13			; return failure count
	FRAME_LEAVE
	POP_NONVOLATILE
	ret

end if ; FP_TEST_FRAMEWORK_INC
