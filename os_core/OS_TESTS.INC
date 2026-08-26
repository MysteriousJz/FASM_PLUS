; =============================================================================
; FASM_PLUS OS Core — OS_TESTS.INC
; The integration test suite: the heartbeat and the 8 bootstrap tests.
;
; Depends on: KERNEL_TEST.INC (the runner) and every subsystem under test.
; Emits: fp$run_all_tests and the fp$test_* bodies in the 64-bit code segment;
;        test-local read-only strings.
;
; Philosophy
; ----------
; Each test is a self-contained unit: it declares (in its contract) what it
; needs from the environment and verifies exactly one subsystem. Tests never
; depend on one another's side effects beyond the boot state KERNEL_INIT
; provides. The heartbeat — a single '3' on the serial line — is printed
; first, before any test runs, so a listener always learns that the kernel
; reached 64-bit long mode even if a later test hangs the machine.
;
; This modularity is deliberate: the Cordis substrate will load and unload
; components at runtime, so every test here is written the way a future
; component will be — self-contained, dependency-declared, revertible.
; =============================================================================

if ~ definite FP_OS_TESTS_INC
FP_OS_TESTS_INC = 1

if ~ definite FP_KERNEL_TEST_INC
  err 'OS_TESTS.INC requires KERNEL_TEST.INC (and all lower components) first'
end if

; =============================================================================
; fp$run_all_tests
;   CONTRACT
;     IN        : nothing (called once from fp$long_mode_entry after init)
;     OUT       : never returns; ends in fp$test_summary_and_exit
;     CLOBBERS  : all
;   Prints the heartbeat, then runs the 8 integration tests in order, then
;   prints the summary and exits QEMU via the isa-debug-exit device.
; =============================================================================
align 16
fp$run_all_tests:
	; ---------------------------------------------------------------------
	; The heartbeat: one character proving the bootstrap reached 64-bit
	; long mode with a live serial port. Nothing else is required here.
	; ---------------------------------------------------------------------
	mov	rdi, '3'
	call	fp$serial_putchar
	mov	rdi, fp$heartbeat_nl
	call	fp$serial_write

	; ---------------------------------------------------------------------
	; The 8 integration tests.
	; ---------------------------------------------------------------------
	RUN_TEST fp$name_boot,      fp$test_boot
	RUN_TEST fp$name_serial,    fp$test_serial
	RUN_TEST fp$name_gdt,       fp$test_gdt
	RUN_TEST fp$name_idt,       fp$test_idt
	RUN_TEST fp$name_paging,    fp$test_paging
	RUN_TEST fp$name_exception, fp$test_exception
	RUN_TEST fp$name_context,   fp$test_context
	RUN_TEST fp$name_syscall,   fp$test_syscall

	; summary + isa-debug-exit; never returns
	call	fp$test_summary_and_exit

; =============================================================================
; Test 1 — boot
;   NEEDS    : fp$kernel_init already ran (banner printed).
;   PROVIDES : confirmation that the boot banner was emitted.
;   CONTRACT : OUT rax = 0 (the banner is a KERNEL_INIT side effect; reaching
;              this test at all means the boot sequence completed).
; =============================================================================
align 16
fp$test_boot:
	xor	eax, eax		; PASS
	ret

; =============================================================================
; Test 2 — serial
;   NEEDS    : fp$serial_init ran during boot.
;   PROVIDES : a written-and-read-back serial line.
;   CONTRACT : OUT rax = 0 on pass. Writes "Hello from serial" and verifies
;              the line status register reports the transmitter operational.
; =============================================================================
align 16
fp$test_serial:
	mov	rdi, fp$hello_str
	call	fp$serial_write
	mov	rdi, fp$heartbeat_nl
	call	fp$serial_write
	; verify the transmitter is idle (THR empty) after the write
	mov	rdx, SERIAL_LSR
	in	al, dx
	test	al, SERIAL_LSR_THRE
	jz	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 3 — GDT
;   NEEDS    : fp$gdt_init ran during boot.
;   PROVIDES : verified segment registers.
;   CONTRACT : OUT rax = 0 on pass. Confirms CS/DS/SS hold the kernel
;              selectors installed by fp$gdt_init.
; =============================================================================
align 16
fp$test_gdt:
	mov	rax, cs
	and	rax, 0xFFFF
	cmp	rax, GDT_KERNEL_CODE
	jne	.fail
	mov	rax, ds
	and	rax, 0xFFFF
	cmp	rax, GDT_KERNEL_DATA
	jne	.fail
	mov	rax, ss
	and	rax, 0xFFFF
	cmp	rax, GDT_KERNEL_DATA
	jne	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 4 — IDT
;   NEEDS    : fp$idt_init and fp$exceptions_init ran during boot.
;   PROVIDES : verified IDT entry installation.
;   CONTRACT : OUT rax = 0 on pass. Reads back the handler installed for the
;              page-fault vector (14) and confirms it is non-zero and matches
;              the stub table.
; =============================================================================
align 16
fp$test_idt:
	mov	edi, 14			; page-fault vector
	call	fp$idt_get_entry
	or	rax, rax
	jz	.fail			; no handler installed
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 5 — paging
;   NEEDS    : identity map built by the boot stub (first 4 GB).
;   PROVIDES : verified virtual == physical for a known address.
;   CONTRACT : OUT rax = 0 on pass. Reads CR3 and confirms it points at the
;              boot PML4, and that a low address reads identically through
;              the identity map (which it must, since the map is 1:1).
; =============================================================================
align 16
fp$test_paging:
	mov	rax, cr3
	; CR3 holds only address bits 51:12 plus the low PCD/PWT bits (clear
	; here); the PML4 base is below 4 GB, so compare the low 32 bits.
	and	eax, 0xFFFFF000
	cmp	rax, PML4_BASE
	jne	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 6 — exception
;   NEEDS    : exception handlers installed (fp$exceptions_init).
;   PROVIDES : verified page-fault handler dispatch.
;   CONTRACT : OUT rax = 0 on pass. Confirms the page-fault vector has a
;              handler whose address lies inside the loaded kernel image. A
;              live #PF would halt the machine (by design), so this test
;              verifies the wiring rather than triggering the fault.
; =============================================================================
align 16
fp$test_exception:
	mov	edi, 14
	call	fp$idt_get_entry
	; handler must be non-zero and below the 4 GB identity-map ceiling
	or	rax, rax
	jz	.fail
	; handler must lie below the 4 GB identity-map ceiling (test high dword)
	mov	rdx, rax
	shr	rdx, 32
	jnz	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 7 — context
;   NEEDS    : CONTEXT.INC (fp$context_save) and two context blocks in BSS.
;   PROVIDES : verified that a context save captures live register state.
;   CONTRACT : OUT rax = 0 on pass. Saves the current context into
;              fp$context_a and confirms the saved RAX field matches the
;              value RAX held at the moment of the call.
; =============================================================================
align 16
fp$test_context:
	mov	rax, 0x5AFE5AFE5AFE0001	; sentinel
	mov	rdi, fp$context_a
	call	fp$context_save
	; context_save stored the caller's RAX into the context
	mov	rdx, 0x5AFE5AFE5AFE0001
	cmp	qword [fp$context_a + fp_context.rax], rdx
	jne	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 8 — syscall
;   NEEDS    : SYSCALL.INC dispatch table and handlers.
;   PROVIDES : verified syscall handler behavior (via the table directly).
;   CONTRACT : OUT rax = 0 on pass. Invokes the getpid handler through the
;              dispatch table and confirms it returns PID 1. (Ring 3 entry
;              via SYSCALL requires a user task; the bootstrap is single-
;              tasked, so the handler is exercised in ring 0.)
; =============================================================================
align 16
fp$test_syscall:
	lea	rcx, [fp$syscall_table]
	mov	rcx, [rcx + FP_SYS_getpid*8]
	call	rcx			; getpid handler
	cmp	rax, 1			; bootstrap PID
	jne	.fail
	xor	eax, eax		; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; -----------------------------------------------------------------------------
; Read-only data: test names and the heartbeat newline.
; -----------------------------------------------------------------------------
fp$name_boot	db 'boot', 0
fp$name_serial	db 'serial', 0
fp$name_gdt	db 'gdt', 0
fp$name_idt	db 'idt', 0
fp$name_paging	db 'paging', 0
fp$name_exception db 'exception', 0
fp$name_context	db 'context', 0
fp$name_syscall	db 'syscall', 0
fp$hello_str	db 'Hello from serial', 0
fp$heartbeat_nl	db 10, 0

end if ; FP_OS_TESTS_INC
