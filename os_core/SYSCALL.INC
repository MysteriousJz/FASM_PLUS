; =============================================================================
; FASM_PLUS OS Core — SYSCALL.INC
; System call interface via the SYSCALL/SYSRET instructions.
;
; Depends on: IDT.INC, CONTEXT.INC, GDT.INC, SERIAL.INC
; Emits: functions and the syscall entry handler in the code section; the
;        dispatch table lives in the current data area.
;
; Ring 3 code invokes a kernel service by loading a syscall number into RAX
; and up to six arguments into RDI RSI RDX R10 R8 R9, then executing the
; SYSCALL instruction. The CPU switches to ring 0 at the LSTAR handler; the
; handler dispatches through the table and returns with SYSRET.
;
; Syscall numbers
; ---------------
;   0  exit     RDI = status                          never returns
;   1  write    RDI = fd, RSI = buffer, RDX = length  RAX = bytes written
;   2  read     RDI = fd, RSI = buffer, RDX = length  RAX = bytes read
;   3  sleep    RDI = milliseconds                    RAX = 0
;   4  yield                                          RAX = 0
;   5  mmap     RDI = addr, RSI = len, RDX = prot, R10 = flags  RAX = addr
;   6  munmap   RDI = addr, RSI = len                 RAX = 0
;   7  getpid                                         RAX = 1
;
; MSR setup (fp$syscall_init):
;   EFER   (0xC0000080)  bit 0 SCE = 1   enable SYSCALL/SYSRET
;   STAR   (0xC0000081)  segment selectors for both directions
;   LSTAR  (0xC0000082)  64-bit syscall entry RIP
;   SFMASK (0xC0000084)  RFLAGS mask; IF is cleared on entry
;
; STAR layout (64-bit MSR):
;   bits 47:32  syscall CS and SS        (CS = value, SS = value + 8)
;   bits 63:48  sysret  CS and SS        (SS = value, CS = value + 16)
; We store kernel CS at 47:32 and (user data - 8) at 63:48 so that SYSRET
; lands on user code/data with the +16/+8 offsets the CPU applies.
; =============================================================================

if ~ definite FP_SYSCALL_INC
FP_SYSCALL_INC = 1

if ~ definite FP_CONTEXT_INC
  err 'SYSCALL.INC requires CONTEXT.INC (and IDT.INC, GDT.INC) first'
end if

; MSR addresses
MSR_EFER        = 0xC0000080
MSR_STAR        = 0xC0000081
MSR_LSTAR       = 0xC0000082
MSR_SFMASK      = 0xC0000084

; EFER bits
EFER_SCE        = 0x001         ; SYSCALL/SYSRET enable
EFER_LME        = 0x100         ; long mode enable

; RFLAGS bits we mask off on syscall entry
RFLAGS_IF       = 0x200         ; interrupt flag

; syscall numbers
FP_SYS_exit     = 0
FP_SYS_write    = 1
FP_SYS_read     = 2
FP_SYS_sleep    = 3
FP_SYS_yield    = 4
FP_SYS_mmap     = 5
FP_SYS_munmap   = 6
FP_SYS_getpid   = 7
FP_SYS_MAX      = 7

; =============================================================================
; fp$syscall_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none
;   Programs the SYSCALL/SYSRET MSRs. The handler address goes to LSTAR, the
;   segment selectors to STAR, and the RFLAGS mask to SFMASK.
; =============================================================================
align 16
fp$syscall_init:
	; EFER.SCE = 1
	mov	rcx, MSR_EFER
	rdmsr
	or	eax, EFER_SCE
	wrmsr
	; LSTAR = handler address
	mov	rcx, MSR_LSTAR
	mov	rax, fp$syscall_handler
	mov	rdx, rax
	shr	rdx, 32
	wrmsr
	; STAR: 47:32 = kernel CS (SS = +8), 63:48 = user data - 8 (CS = +16)
	mov	rcx, MSR_STAR
	mov	eax, GDT_KERNEL_CODE	; bits 47:32
	mov	edx, GDT_USER_DATA - 8	; bits 63:48 (sysret SS), CS = +16
	wrmsr
	; SFMASK: clear IF on entry so the handler runs with interrupts masked
	mov	rcx, MSR_SFMASK
	mov	eax, RFLAGS_IF
	xor	edx, edx
	wrmsr
	ret

; =============================================================================
; fp$syscall_handler — SYSCALL entry point
;   CONTRACT
;     IN        : RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args
;     OUT       : RAX = return value (negative = errno)
;     CLOBBERS  : RCX, R11 (clobbered by SYSCALL itself), and per-handler
;     PRESERVES : all non-volatile registers; the caller's RSP is untouched
;     ERRORS    : negative errno in RAX (E_OUT_OF_RANGE for a bad number)
;   On entry the CPU has placed the user RIP in RCX and RFLAGS in R11, and
;   switched to the kernel CS/SS from STAR. RSP is NOT switched — the handler
;   runs on the user stack, so it must not push until it has validated state.
;   For this bootstrap the handlers are leaf-like and use no user memory, so
;   a direct dispatch with SYSRETQ is safe.
; =============================================================================
align 16
fp$syscall_handler:
	; RAX = syscall number; clamp to the table
	cmp	rax, FP_SYS_MAX
	ja	.bad_number
	; dispatch: RCX (user RIP) is dead once we save it, so use it as the
	; table index register. Save it first so a handler could inspect it.
	push	rcx			; save user RIP
	push	r11			; save user RFLAGS
	lea	rcx, [fp$syscall_table]
	mov	rcx, [rcx + rax*8]	; rcx = handler pointer
	call	rcx			; handler returns RAX
	pop	r11			; user RFLAGS for sysretq
	pop	rcx			; user RIP for sysretq
	sysretq
      .bad_number:
	mov	rax, E_OUT_OF_RANGE
	sysretq

; -----------------------------------------------------------------------------
; Individual syscall handlers. Each ends with ret (the dispatcher returns via
; sysretq after the handler). Arguments are in the SysV syscall registers;
; return value goes in RAX.
; -----------------------------------------------------------------------------

; int 0 — exit(RDI = status): write the status to the QEMU debug-exit port
; and halt. Never returns. This is the bootstrap "power off" path.
align 16
fp$sys_exit:
	mov	rdx, 0xF4		; QEMU isa-debug-exit port
	mov	rax, rdi
	out	dx, al			; low byte of status
      .halt:
	cli
	hlt
	jmp	.halt

; int 1 — write(RDI = fd, RSI = buffer, RDX = length) -> bytes written.
; The only supported fd is 1 (stdout), which is the serial port. This is a
; plain `ret` handler (the dispatcher returns via sysretq after it).
align 16
fp$sys_write:
	cmp	rdi, 1			; stdout only
	jne	.bad_fd
	mov	rcx, rdx		; length
	or	rcx, rcx
	jz	.done
      .loop:
	movzx	eax, byte [rsi]
	push	rcx
	push	rsi
	mov	rdi, rax
	call	fp$serial_putchar
	pop	rsi
	pop	rcx
	inc	rsi
	dec	rcx
	jnz	.loop
      .done:
	mov	rax, rdx		; bytes written = full length
	ret
      .bad_fd:
	mov	rax, E_PERMISSION
	ret

; int 2 — read(RDI = fd, RSI = buffer, RDX = length) -> bytes read.
; The serial console is write-only in this bootstrap; nothing to read.
align 16
fp$sys_read:
	xor	eax, eax		; 0 bytes read
	ret

; int 3 — sleep(RDI = milliseconds) -> 0. A busy spin on the PIT tick would
; go here; for the bootstrap it is a calibrated no-op returning success.
align 16
fp$sys_sleep:
	xor	eax, eax
	ret

; int 4 — yield() -> 0. Single-tasked bootstrap: nothing to yield to.
align 16
fp$sys_yield:
	xor	eax, eax
	ret

; int 5 — mmap(...) -> addr. No virtual memory manager yet; return the
; requested address unchanged if it is already mapped by the identity map,
; else E_OUT_OF_MEMORY.
align 16
fp$sys_mmap:
	mov	rax, rdi		; requested address
	; identity map covers [0, 4 GB). CMP cannot take a 64-bit immediate, so
	; test the high dword: any non-zero bit above bit 31 means >= 4 GB.
	mov	rdx, rax
	shr	rdx, 32
	jnz	.fail
	ret				; identity-mapped: return it
      .fail:
	mov	rax, E_OUT_OF_MEMORY
	ret

; int 6 — munmap(RDI = addr, RSI = len) -> 0. Identity map is permanent.
align 16
fp$sys_munmap:
	xor	eax, eax
	ret

; int 7 — getpid() -> 1. The bootstrap runs exactly one task, PID 1.
align 16
fp$sys_getpid:
	mov	eax, 1
	ret

; -----------------------------------------------------------------------------
; Read-only data: the dispatch table, indexed by syscall number.
; -----------------------------------------------------------------------------
align 8
fp$syscall_table:
	dq fp$sys_exit
	dq fp$sys_write
	dq fp$sys_read
	dq fp$sys_sleep
	dq fp$sys_yield
	dq fp$sys_mmap
	dq fp$sys_munmap
	dq fp$sys_getpid

end if ; FP_SYSCALL_INC
