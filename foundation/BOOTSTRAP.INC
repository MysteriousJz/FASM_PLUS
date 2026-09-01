; =============================================================================
; FASM_PLUS Foundation Library — Layer 4/bootstrap
; BOOTSTRAP.INC — program initialization and clean shutdown
;
; Depends on: CONSTANTS, CONTRACTS, STATE_MANAGEMENT, MEMORY_PRIMITIVES,
;             IO_BASIC, TEST_FRAMEWORK (for fp$boot_flags bits), CRASH_DETECTOR
;             (optional; included by the consumer if desired)
; Emits: functions in the current code section; state in FOUNDATION_DATA.INC
;
; Initialization sequence (fp$init):
;   1. Force 16-byte stack alignment (entry alignment is not guaranteed).
;   2. Record the initial stack top for later overflow sanity checks.
;   3. Detect basic CPU features (via CPUID) and record them.
;   4. Verify the stack is writable where expected (canary round-trip).
;   5. Mark fp$boot_flags.initialized.
;   Failure at any step exits with EXIT_BOOTSTRAP_FAIL — a controlled halt,
;   never a limp-along.
;
; Shutdown sequence (fp$exit / fp$shutdown):
;   * optional test summary already printed by the runner
;   * exit_group with the supplied code (no atexit, no flush — we have no
;     buffered state)
; =============================================================================

if ~ definite FP_BOOTSTRAP_INC
FP_BOOTSTRAP_INC = 1

if ~ definite FP_TEST_FRAMEWORK_INC
  err 'BOOTSTRAP.INC requires TEST_FRAMEWORK.INC (and all lower layers) first'
end if

; boot flag bits
FP_BOOT_INITIALIZED = 1
FP_BOOT_CRASH_ARMED = 2

; =============================================================================
; fp$cpuid_check — detect a minimal CPU feature set and stash it
;   CONTRACT
;     IN        : none
;     OUT       : rax = feature word (bit0 SSE, bit1 SSE2, bit2 SSE3,
;                 bit3 SSE4.1, bit4 AVX from CPUID.1 feature bits); also
;                 stored in fp$cpu_features
;     CLOBBERS  : rax rbx rcx rdx flags
;     ERRORS    : none; on a CPU without CPUID (not possible on x86-64) the
;                 result is 0
;   NOTE: x86-64 mandates CPUID, SSE and SSE2, so bits 0/1 are sanity checks
;   rather than real discovery — a failure here means the environment is not
;   really x86-64 and we halt in fp$init.
; =============================================================================
align 16
fp$cpuid_check:
	push	rbx				; CPUID clobbers rbx (non-volatile)
	mov	eax, 1
	xor	ecx, ecx
	cpuid
	; edx feature bits: 25=SSE 26=SSE2 ; ecx: 0=SSE3 19=SSE4.1 28=AVX
	xor	eax, eax
	bt	edx, 25
	adc	eax, 0				; bit0 SSE
	bt	edx, 26
	adc	eax, 0				; -> bit1 via carry after shl below
	; build the word explicitly to keep bit order deterministic
	xor	eax, eax
	bt	edx, 25
	setc	al
	bt	edx, 26
	setc	ah				; bit1 in ah
	movzx	r8d, ax				; bits 0..1
	xor	eax, eax
	bt	ecx, 0				; SSE3
	setc	al
	shl	eax, 2
	or	r8d, eax
	xor	eax, eax
	bt	ecx, 19				; SSE4.1
	setc	al
	shl	eax, 3
	or	r8d, eax
	xor	eax, eax
	bt	ecx, 28				; AVX
	setc	al
	shl	eax, 4
	or	r8d, eax
	mov	eax, r8d
	mov	[fp$cpu_features], rax
	pop	rbx
	ret

; =============================================================================
; fp$init — run the bootstrap sequence
;   CONTRACT
;     IN        : none
;     OUT       : rax = E_OK on success; on failure does NOT return (exits
;                 with EXIT_BOOTSTRAP_FAIL)
;     CLOBBERS  : rax rbx rcx rdx rsi rdi flags; records stack top and CPU
;                 features
;     ERRORS    : halts on stack-misalignment sanity failure or missing
;                 mandatory CPU features
; =============================================================================
align 16
fp$init:
	; Establish a frame FIRST so the return address stays reachable. We then
	; align rsp within the frame for any SSE work our callees might do. The
	; frame is torn down before ret, restoring the caller's rsp exactly.
	push	rbp
	mov	rbp, rsp
	and	rsp, not (ALIGN_STACK-1)
	mov	[fp$boot_stack_top], rsp

	; stack canary round-trip: prove the stack is writable and that our
	;    canary constant survives a store/load. A fault here means the stack
	;    is not usable and we must halt.
	mov	rax, STACK_CANARY_VALUE
	mov	[rsp-16], rax
	cmp	[rsp-16], rax
	jne	.fail

	; CPU features; require SSE and SSE2 (x86-64 baseline)
	call	fp$cpuid_check
	test	rax, 0x3			; bits 0 and 1
	jz	.fail

	; record initialization
	or	qword [fp$boot_flags], FP_BOOT_INITIALIZED
	mov	rax, E_OK
	mov	rsp, rbp			; restore caller's stack
	pop	rbp
	ret
.fail:
	mov	eax, SYS_exit_group
	mov	edi, EXIT_BOOTSTRAP_FAIL
	syscall

; =============================================================================
; fp$exit — clean shutdown
;   CONTRACT
;     IN        : rdi = exit code
;     OUT       : does not return
;     CLOBBERS  : rax rdi rcx r11
;     ERRORS    : none
;   Uses exit_group so all threads (if a future layer adds them) terminate.
; =============================================================================
align 16
fp$exit:
	mov	eax, SYS_exit_group
	syscall
	; exit_group does not return; the ud2 is a defensive trap if it ever did
	ud2

; =============================================================================
; fp$halt_error — controlled halt with a distinctive code and message
;   CONTRACT
;     IN        : rdi = exit code, rsi = NUL-terminated message
;     OUT       : writes "<msg>\n" to stderr, then exits with the code
;     CLOBBERS  : many; terminal path
;     ERRORS    : none; always halts
; =============================================================================
align 16
fp$halt_error:
	push	rdi
	; write message to stderr
	mov	rdx, -1
.len:
	inc	rdx
	cmp	byte [rsi+rdx], 0
	jne	.len
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	syscall
	; newline
	mov	byte [rsp-8], 10
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	pop	rdi
	mov	eax, SYS_exit_group
	syscall
	ud2

end if ; FP_BOOTSTRAP_INC
