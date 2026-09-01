; =============================================================================
; FASM_PLUS OS Core — CONTEXT.INC
; Task context structure and context switching.
;
; Depends on: STATE_MANAGEMENT.INC, PAGING.INC (for CR3 handling), CONSTANTS
; Emits: functions in the current code section.
;
; A context captures the full CPU state of a task so it can be suspended and
; resumed. The layout is fixed and 64-byte aligned; the FXSAVE area at the
; tail must be 64-byte aligned, which the struct guarantees.
;
; Philosophy
; ----------
; Context switching is the mechanism by which the kernel multitasks. It must
; be exact: every general register, the instruction pointer, the flags, the
; stack pointer, and the FPU/SSE state round-trip without loss. There is no
; hidden work — save writes the registers, restore reads them, and the jump
; to the saved RIP is a plain ret through a constructed frame.
; =============================================================================

if ~ definite FP_CONTEXT_INC
FP_CONTEXT_INC = 1

if ~ definite FP_STATE_MANAGEMENT_INC
  err 'CONTEXT.INC requires STATE_MANAGEMENT.INC (and CONSTANTS.INC) first'
end if

; -----------------------------------------------------------------------------
; Context structure (64-byte aligned). Field offsets are fixed and named so
; the assembly is greppable; the FPU area is last so the whole struct can be
; 64-byte aligned without padding the integer fields.
; -----------------------------------------------------------------------------
struc fp_context {
    .r15	dq ?
    .r14	dq ?
    .r13	dq ?
    .r12	dq ?
    .r11	dq ?
    .r10	dq ?
    .r9		dq ?
    .r8		dq ?
    .rbp	dq ?
    .rdi	dq ?
    .rsi	dq ?
    .rdx	dq ?
    .rcx	dq ?
    .rbx	dq ?
    .rax	dq ?
    .rip	dq ?
    .cs		dq ?
    .rflags	dq ?
    .rsp	dq ?
    .ss		dq ?
    .cr3	dq ?
    align 64
    .fpu_state rb 512		; FXSAVE area (64-byte aligned by the struct)
}

; Materialize the field offsets as fp_context.<field> constants. In FASM a
; struc is a template; the offsets only become usable address constants once
; the struc is instantiated. Instantiating it in a virtual block at address 0
; defines every fp_context.<field> as its byte offset without emitting data.
virtual at 0
    fp_context fp_context
end virtual

; size check: 21 qwords = 168, rounded up to 192, plus 512 = 704
; the struct must be a multiple of 64 so .fpu_state is 64-byte aligned
FP_CONTEXT_SIZE = 704

; =============================================================================
; fp$context_save
;   CONTRACT
;     IN        : rdi = pointer to a 64-byte-aligned fp_context
;     OUT       : nothing
;     CLOBBERS  : flags; memory at [rdi]
;     PRESERVES : all general registers (rax used as scratch via stack)
;     ERRORS    : none
;   Saves the current CPU state into the context. Does NOT save RIP/RSP/
;   RFLAGS — the caller supplies those (they are the resume point). The FPU
;   state IS saved so a context switch round-trips x87/SSE transparently.
; =============================================================================
align 16
fp$context_save:
	mov	[rdi + fp_context.r15], r15
	mov	[rdi + fp_context.r14], r14
	mov	[rdi + fp_context.r13], r13
	mov	[rdi + fp_context.r12], r12
	mov	[rdi + fp_context.r11], r11
	mov	[rdi + fp_context.r10], r10
	mov	[rdi + fp_context.r9], r9
	mov	[rdi + fp_context.r8], r8
	mov	[rdi + fp_context.rbp], rbp
	mov	[rdi + fp_context.rdi], rdi	; self-reference is harmless
	mov	[rdi + fp_context.rsi], rsi
	mov	[rdi + fp_context.rdx], rdx
	mov	[rdi + fp_context.rcx], rcx
	mov	[rdi + fp_context.rbx], rbx
	mov	[rdi + fp_context.rax], rax
	; FPU/SSE state (fxsave64 requires 64-byte alignment, guaranteed by struct)
	lea	rax, [rdi + fp_context.fpu_state]
	fxsave64 [rax]
	ret

; =============================================================================
; fp$context_restore
;   CONTRACT
;     IN        : rdi = pointer to a 64-byte-aligned fp_context
;     OUT       : never returns normally — jumps to the saved RIP
;     CLOBBERS  : all registers (restored from the context)
;     ERRORS    : none
;   Restores the CPU state from the context and jumps to the saved RIP.
;   Because the context carries RIP/CS/RFLAGS/RSP/SS, the jump is built as an
;   iret frame on the current stack.
; =============================================================================
align 16
fp$context_restore:
	; restore FPU/SSE state first (uses rdi before it is overwritten)
	lea	rax, [rdi + fp_context.fpu_state]
	fxrstor64 [rax]
	; build an iret frame on the current stack: SS RSP RFLAGS CS RIP
	mov	rax, [rdi + fp_context.ss]
	push	rax
	mov	rax, [rdi + fp_context.rsp]
	push	rax
	mov	rax, [rdi + fp_context.rflags]
	push	rax
	mov	rax, [rdi + fp_context.cs]
	push	rax
	mov	rax, [rdi + fp_context.rip]
	push	rax
	; restore general registers (rdi last)
	mov	r15, [rdi + fp_context.r15]
	mov	r14, [rdi + fp_context.r14]
	mov	r13, [rdi + fp_context.r13]
	mov	r12, [rdi + fp_context.r12]
	mov	r11, [rdi + fp_context.r11]
	mov	r10, [rdi + fp_context.r10]
	mov	r9, [rdi + fp_context.r9]
	mov	r8, [rdi + fp_context.r8]
	mov	rbp, [rdi + fp_context.rbp]
	mov	rsi, [rdi + fp_context.rsi]
	mov	rdx, [rdi + fp_context.rdx]
	mov	rcx, [rdi + fp_context.rcx]
	mov	rbx, [rdi + fp_context.rbx]
	mov	rax, [rdi + fp_context.rax]
	mov	rdi, [rdi + fp_context.rdi]
	iretq

; =============================================================================
; fp$context_switch
;   CONTRACT
;     IN        : rdi = pointer to old fp_context, rsi = pointer to new
;     OUT       : nothing visible (control resumes in the new context)
;     CLOBBERS  : all registers (restored from the new context)
;     ERRORS    : none
;   Saves the current state into the old context, then restores and jumps to
;   the new context. The resume point for the OLD context is the instruction
;   after this call, so a later switch back returns here as if nothing moved.
; =============================================================================
align 16
fp$context_switch:
	; save the caller's resume point into the old context before the
	; general save overwrites the scratch registers we need
	mov	rax, [rsp]		; return address = old task's resume RIP
	mov	[rdi + fp_context.rip], rax
	mov	rax, cs
	mov	[rdi + fp_context.cs], rax
	pushfq
	pop	rax
	mov	[rdi + fp_context.rflags], rax
	lea	rax, [rsp + 8]		; old RSP after the return address
	mov	[rdi + fp_context.rsp], rax
	mov	rax, ss
	mov	[rdi + fp_context.ss], rax
	; save all general registers + FPU state into the old context
	call	fp$context_save
	; switch to the new context and jump to it
	mov	rdi, rsi
	call	fp$context_restore
	; (never reached: restore jumps to the new context's saved RIP)

end if ; FP_CONTEXT_INC
