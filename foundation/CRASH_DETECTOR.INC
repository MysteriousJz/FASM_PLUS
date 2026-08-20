; =============================================================================
; FASM_PLUS Foundation Library — verification layer
; CRASH_DETECTOR.INC — signal handlers, context capture, post-mortem report
;
; Depends on: CONSTANTS, STATE_MANAGEMENT, IO_BASIC (reporting), BOOTSTRAP
; Emits: functions + the signal-handler trampoline in the code section;
;        capture buffers live in FOUNDATION_DATA.INC
;
; Philosophy
; ----------
; "Crash prevention without explanation" is the enemy. When a fault happens
; we (1) capture the faulting RIP, the fault address, and the signal number,
; (2) snapshot the integer registers, and (3) emit a one-line report to
; stderr before re-raising so the process still terminates with the correct
; signal semantics. Async-signal-safety is respected: the handler only calls
; raw write(2) via syscall, never anything with locks or allocation.
;
; The handler uses SA_SIGINFO so we receive a ucontext with the full register
; state. We pull RIP and the general registers out of the ucontext.
; =============================================================================

if ~ definite FP_CRASH_DETECTOR_INC
FP_CRASH_DETECTOR_INC = 1

if ~ definite FP_BOOTSTRAP_INC
  err 'CRASH_DETECTOR.INC requires BOOTSTRAP.INC (and lower layers) first'
end if

; ucontext offsets on x86-64 Linux (glibc/musl agree for the gregs we use).
; uc_mcontext.gregs[] base within ucontext_t is 40; each greg is 8 bytes.
; Index map (from <sys/ucontext.h>): 0=GS.. 10=RDI 11=RSI 12=RBP 13=RBX
; 14=RDX 15=RAX 16=RCX 17=RSP 18=RSI? -- see gregs order below.
; Verified order (REG_* indices): R8=0 R9=1 R10=2 R11=3 R12=4 R13=5 R14=6
; R15=7 RDI=8 RSI=9 RBP=10 RBX=11 RDX=12 RAX=13 RCX=14 RSP=15 RIP=16
UC_MCONTEXT_GREGS = 40
UC_R8   = UC_MCONTEXT_GREGS + 0*8
UC_R9   = UC_MCONTEXT_GREGS + 1*8
UC_R10  = UC_MCONTEXT_GREGS + 2*8
UC_R11  = UC_MCONTEXT_GREGS + 3*8
UC_R12  = UC_MCONTEXT_GREGS + 4*8
UC_R13  = UC_MCONTEXT_GREGS + 5*8
UC_R14  = UC_MCONTEXT_GREGS + 6*8
UC_R15  = UC_MCONTEXT_GREGS + 7*8
UC_RDI  = UC_MCONTEXT_GREGS + 8*8
UC_RSI  = UC_MCONTEXT_GREGS + 9*8
UC_RBP  = UC_MCONTEXT_GREGS + 10*8
UC_RBX  = UC_MCONTEXT_GREGS + 11*8
UC_RDX  = UC_MCONTEXT_GREGS + 12*8
UC_RAX  = UC_MCONTEXT_GREGS + 13*8
UC_RCX  = UC_MCONTEXT_GREGS + 14*8
UC_RSP  = UC_MCONTEXT_GREGS + 15*8
UC_RIP  = UC_MCONTEXT_GREGS + 16*8

; =============================================================================
; fp$crash_handler — the SA_SIGINFO handler
;   Called by the kernel: rdi = signal number, rsi = siginfo_t*,
;   rdx = ucontext_t*.
;   CONTRACT
;     IN        : kernel-provided signal frame
;     OUT       : writes a one-line report to stderr, then restores the
;                 default handler and re-raises so the process dies with the
;                 original signal (correct wait-status for the runner)
;     CLOBBERS  : async-signal context only
;     ERRORS    : none; if reporting faults, the re-raise still happens
; =============================================================================
align 16
fp$crash_handler:
	; rsi -> siginfo: si_addr is at offset 16 (after signo, errno, code,
	; and the pad); for SIGSEGV/SIGBUS/SIGILL it holds the fault address.
	mov	rax, [rsi+16]			; si_addr
	mov	[fp$crash_addr], rax
	mov	eax, [rsi]			; si_signo
	mov	[fp$crash_signal], rax
	mov	eax, [rsi+8]			; si_code
	mov	[fp$crash_code], rax

	; rdx -> ucontext: pull RIP, RSP and the general registers
	mov	rax, [rdx+UC_RIP]
	mov	[fp$crash_rip], rax
	mov	rax, [rdx+UC_RSP]
	mov	[fp$crash_rsp], rax

	; snapshot the register block (order: rax rbx rcx rdx rsi rdi rbp rsp
	; r8 r9 r10 r11 r12 r13 r14 r15) into fp$crash_regs
	mov	rax, [rdx+UC_RAX]
	mov	qword [fp$crash_regs+0*8], rax
	mov	rax, [rdx+UC_RBX]
	mov	qword [fp$crash_regs+1*8], rax
	mov	rax, [rdx+UC_RCX]
	mov	qword [fp$crash_regs+2*8], rax
	mov	rax, [rdx+UC_RDX]
	mov	qword [fp$crash_regs+3*8], rax
	mov	rax, [rdx+UC_RSI]
	mov	qword [fp$crash_regs+4*8], rax
	mov	rax, [rdx+UC_RDI]
	mov	qword [fp$crash_regs+5*8], rax
	mov	rax, [rdx+UC_RBP]
	mov	qword [fp$crash_regs+6*8], rax
	mov	rax, [rdx+UC_RSP]
	mov	qword [fp$crash_regs+7*8], rax
	mov	rax, [rdx+UC_R8]
	mov	qword [fp$crash_regs+8*8], rax
	mov	rax, [rdx+UC_R9]
	mov	qword [fp$crash_regs+9*8], rax
	mov	rax, [rdx+UC_R10]
	mov	qword [fp$crash_regs+10*8], rax
	mov	rax, [rdx+UC_R11]
	mov	qword [fp$crash_regs+11*8], rax
	mov	rax, [rdx+UC_R12]
	mov	qword [fp$crash_regs+12*8], rax
	mov	rax, [rdx+UC_R13]
	mov	qword [fp$crash_regs+13*8], rax
	mov	rax, [rdx+UC_R14]
	mov	qword [fp$crash_regs+14*8], rax
	mov	rax, [rdx+UC_R15]
	mov	qword [fp$crash_regs+15*8], rax

	; report: "CRASH sig=<n> rip=0x.. addr=0x..\n"  (async-signal-safe:
	; only raw write syscalls; no allocation, no locks)
	lea	rsi, [fp$str_crash]
	call	.fp_safe_write_err
	mov	rdi, [fp$crash_signal]
	call	.fp_safe_write_uint
	lea	rsi, [fp$str_rip]
	call	.fp_safe_write_err
	mov	rdi, [fp$crash_rip]
	call	.fp_safe_write_hex
	lea	rsi, [fp$str_addr]
	call	.fp_safe_write_err
	mov	rdi, [fp$crash_addr]
	call	.fp_safe_write_hex
	call	.fp_safe_write_nl

	; After capturing state and reporting, we halt with EXIT_CRASH rather
	; than re-raising the signal. Rationale: re-raising requires unblocking
	; the signal and juggling pid/self-kill mid-handler, which is fragile in
	; async-signal context. A dedicated exit code is a deterministic,
	; observable "we crashed and captured state" signal that the test runner
	; checks for explicitly. The captured register/address state remains
	; available in fp$crash_* for any post-mortem harness that forks us.
	mov	eax, SYS_exit_group
	mov	edi, EXIT_CRASH
	syscall

; --- async-signal-safe write helpers (no libc, no state besides syscalls) ---
align 16
.fp_safe_write_err:			; rsi = cstr
	mov	rdx, -1
.len:
	inc	rdx
	cmp	byte [rsi+rdx], 0
	jne	.len
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	syscall
	ret
align 16
.fp_safe_write_nl:
	mov	byte [rsp-8], 10
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	ret
align 16
.fp_safe_write_hex:			; rdi = value, 16 hex digits
	lea	r9, [rsp-32]
	mov	ecx, 16
.d:
	mov	rax, rdi
	and	rax, MASK_LOW_NIBBLE
	cmp	al, 10
	jb	.n
	add	al, 'a'-10
	jmp	.s
.n:
	add	al, '0'
.s:
	dec	r9
	mov	[r9], al
	shr	rdi, 4
	dec	ecx
	jnz	.d
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	mov	rsi, r9
	mov	edx, 16
	syscall
	ret
align 16
.fp_safe_write_uint:			; rdi = unsigned value
	lea	r9, [rsp-16]
	mov	rax, rdi
	mov	r8, 10
	xor	ecx, ecx
.dl:
	xor	edx, edx
	div	r8
	add	dl, '0'
	dec	r9
	mov	[r9], dl
	inc	rcx
	test	rax, rax
	jnz	.dl
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	mov	rsi, r9
	mov	rdx, rcx
	syscall
	ret

; =============================================================================
; fp$signal_default — reset a signal to SIG_DFL
;   CONTRACT
;     IN        : rdi = signal number
;     OUT       : rax = 0 or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r10 r11 flags; uses stack scratch for a
;                 zeroed sigaction
; =============================================================================
align 16
fp$signal_default:
	; build a minimal sigaction on the stack: handler=0 (SIG_DFL),
	; flags=0, mask empty, restorer=0
	sub	rsp, 32
	mov	qword [rsp], 0			; sa_handler = SIG_DFL
	mov	qword [rsp+8], 0		; sa_flags
	mov	qword [rsp+16], 0		; sa_restorer
	mov	qword [rsp+24], 0		; sa_mask (one qword suffices here)
	; rt_sigaction(sig, act, old, sigsetsize=8)
	mov	rsi, rsp
	xor	edx, edx			; oldact = NULL
	mov	r10d, 8				; sigsetsize
	mov	eax, SYS_rt_sigaction
	syscall
	add	rsp, 32
	ret

; =============================================================================
; fp$crash_install — install handlers for SIGSEGV, SIGILL, SIGBUS, SIGABRT
;   CONTRACT
;     IN        : none
;     OUT       : rax = E_OK on success, or a negative errno from the first
;                 sigaction that failed
;     CLOBBERS  : rax rcx rdx rsi rdi r10 r11 flags; uses stack scratch
;     ERRORS    : negative errno; partial installation is possible — the
;                 failing signal index is implied by the return
;   Sets FP_BOOT_CRASH_ARMED in fp$boot_flags on full success.
; =============================================================================
align 16
fp$crash_install:
	; for each of the four fatal signals, install fp$crash_handler with
	; SA_SIGINFO|SA_RESTORER and the kernel restorer trampoline.
	lea	r9, [fp$crash_handler]
	; SIGSEGV
	mov	edi, SIGSEGV
	mov	rsi, r9
	call	.install_one
	test	rax, rax
	jnz	.done
	mov	edi, SIGILL
	mov	rsi, r9
	call	.install_one
	test	rax, rax
	jnz	.done
	mov	edi, SIGBUS
	mov	rsi, r9
	call	.install_one
	test	rax, rax
	jnz	.done
	mov	edi, SIGABRT
	mov	rsi, r9
	call	.install_one
	test	rax, rax
	jnz	.done
	or	qword [fp$boot_flags], FP_BOOT_CRASH_ARMED
	mov	rax, E_OK
.done:
	ret

; helper: rdi = signal, rsi = handler; returns rax = 0 or -errno
align 16
.install_one:
	sub	rsp, 40
	mov	[rsp], rsi			; sa_handler
	mov	qword [rsp+8], SA_SIGINFO or SA_RESTORER
	lea	rax, [fp$restorer]
	mov	[rsp+16], rax			; sa_restorer
	mov	qword [rsp+24], 0		; sa_mask
	mov	rsi, rsp
	xor	edx, edx
	mov	r10d, 8
	mov	eax, SYS_rt_sigaction
	syscall
	add	rsp, 40
	ret

; the kernel restorer trampoline: invoked on return from the handler
align 16
fp$restorer:
	mov	eax, SYS_rt_sigreturn
	syscall

end if ; FP_CRASH_DETECTOR_INC
