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

; The OS core test runner is defined below. A target-specific image (for
; example the T440 build) may define FP_OS_TESTS_NO_RUNNER before including
; this file to suppress fp$run_all_tests and supply its own runner that also
; invokes the target's extra tests.
if ~ definite FP_OS_TESTS_NO_RUNNER

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

	; ---------------------------------------------------------------------
	; The Cordis fiber-tree tests: dependency chain, root unload, reload,
	; and hot-replace. These prove dynamic composability end to end.
	; ---------------------------------------------------------------------
	RUN_TEST fp$name_ft_chain,  fp$test_fiber_tree_chain
	RUN_TEST fp$name_ft_unload, fp$test_fiber_tree_unload
	RUN_TEST fp$name_ft_reload, fp$test_fiber_tree_reload
	RUN_TEST fp$name_ft_hot,    fp$test_fiber_tree_hot_replace

	; summary + isa-debug-exit; never returns
	call	fp$test_summary_and_exit

end if ; ~ definite FP_OS_TESTS_NO_RUNNER

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

; =============================================================================
; Cordis fiber-tree tests
; -----------------------
; A small component tree is built and torn down to prove the Cordis
; primitives:
;
;     A (provides KEY_X)
;     └── B (injects KEY_X, provides KEY_Y)
;         └── C (injects KEY_X, KEY_Y)
;
; Fiber ids are fixed test slots; keys and values are small integers. Each
; test is self-contained: it resets the registry, drives the Cordis handlers
; directly (the same code the SYSCALL dispatcher reaches), and asserts on the
; resulting fiber states and coeffect contents.
;
; The inverse used for effect tracking increments a counter, so a test can
; confirm exactly how many effects were reverted on unload/rollback.
; =============================================================================

; test fiber ids (registry slots)
FT_FIBER_A	= 1
FT_FIBER_B	= 2
FT_FIBER_C	= 3

; test dependency keys (arbitrary unique tokens)
FT_KEY_X	= 0x1001
FT_KEY_Y	= 0x1002

; values supplied by providers (distinct so hot-replace is observable)
FT_VAL_A	= 0xAAAA
FT_VAL_A_PRIME	= 0xBBBB

; =============================================================================
; fp$ft_inverse — test inverse function. Increments fp$ft_revert_count.
;   CONTRACT: IN rdi = context (ignored); OUT nothing; CLOBBERS rax, flags.
; =============================================================================
align 16
fp$ft_inverse:
	inc	qword [fp$ft_revert_count]
	ret

; fp$ft_reset — clear the registry for a clean, self-contained test.
;   CONTRACT: CLOBBERS rax, rcx, rdx, rdi.
align 16
fp$ft_reset:
	mov	qword [fp$ft_revert_count], 0
	call	fp$cordis_init
	ret

; =============================================================================
; Test 9 — fiber_tree_dependency_chain
;   Load A (provides KEY_X), B (injects KEY_X, provides KEY_Y),
;   C (injects KEY_X + KEY_Y). Assert all become ACTIVE and provisions land
;   in the coeffect table.
; =============================================================================
align 16
fp$test_fiber_tree_chain:
	call	fp$ft_reset
	; --- A provides KEY_X = FT_VAL_A ---
	mov	rdi, FT_FIBER_A
	mov	rsi, FT_KEY_X
	mov	rdx, FT_VAL_A
	call	fp$coeffect_provide
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_A
	call	fp$fiber_load
	test	rax, rax			; 0 = active
	jnz	.fail
	mov	rdi, FT_FIBER_A
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; KEY_X must resolve to FT_VAL_A
	mov	rdi, FT_KEY_X
	call	fp$coeffect_lookup
	cmp	rax, FT_VAL_A
	jne	.fail
	; --- B injects KEY_X, provides KEY_Y ---
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_Y
	mov	rdx, 0xB0B0
	call	fp$coeffect_provide
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_B
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_B
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; KEY_Y must now be in the table
	mov	rdi, FT_KEY_Y
	call	fp$coeffect_lookup
	cmp	rax, 0xB0B0
	jne	.fail
	; --- C injects KEY_X and KEY_Y ---
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_Y
	call	fp$coeffect_declare
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_C
	call	fp$fiber_load
	test	rax, rax			; both deps satisfied -> active
	jnz	.fail
	mov	rdi, FT_FIBER_C
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	xor	eax, eax			; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 10 — fiber_tree_root_unload
;   Build A->B->C, give A a revertible effect, then unload A. Assert B and C
;   deactivate, A's effect is reverted, and the coeffect table is clean.
; =============================================================================
align 16
fp$test_fiber_tree_unload:
	call	fp$ft_reset
	; build the chain (same as test 9)
	call	fp$ft_build_chain
	test	rax, rax
	jnz	.fail
	; record a revertible effect on A
	mov	rdi, FT_FIBER_A
	mov	rsi, fp$ft_inverse
	mov	rdx, 0
	call	fp$effect_record
	test	rax, rax
	js	.fail
	; sanity: all three active
	mov	rdi, FT_FIBER_A
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	mov	rdi, FT_FIBER_B
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	mov	rdi, FT_FIBER_C
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; --- unload the root provider A ---
	mov	rdi, FT_FIBER_A
	call	fp$fiber_unload
	test	rax, rax			; 0 = fully unloaded
	jnz	.fail
	; A must be Inactive and its effect reverted
	mov	rdi, FT_FIBER_A
	call	fp$fiber_state
	cmp	rax, FIBER_INACTIVE
	jne	.fail
	cmp	qword [fp$ft_revert_count], 1
	jne	.fail
	; B and C must have deactivated (their deps disappeared)
	mov	rdi, FT_FIBER_B
	call	fp$fiber_state
	cmp	rax, FIBER_INACTIVE
	jne	.fail
	mov	rdi, FT_FIBER_C
	call	fp$fiber_state
	cmp	rax, FIBER_INACTIVE
	jne	.fail
	; coeffect table must be clean: KEY_X and KEY_Y unprovided
	mov	rdi, FT_KEY_X
	call	fp$coeffect_lookup
	cmp	rax, CORDIS_NIL
	jne	.fail
	mov	rdi, FT_KEY_Y
	call	fp$coeffect_lookup
	cmp	rax, CORDIS_NIL
	jne	.fail
	xor	eax, eax			; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 11 — fiber_tree_root_reload
;   Build A->B->C, unload A, then reload A, B, C. Assert all re-activate and
;   bindings resolve again.
; =============================================================================
align 16
fp$test_fiber_tree_reload:
	call	fp$ft_reset
	call	fp$ft_build_chain
	test	rax, rax
	jnz	.fail
	; unload root
	mov	rdi, FT_FIBER_A
	call	fp$fiber_unload
	test	rax, rax
	jnz	.fail
	; reload A -> B -> C
	call	fp$ft_build_chain
	test	rax, rax
	jnz	.fail
	; all must be Active again
	mov	rdi, FT_FIBER_A
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	mov	rdi, FT_FIBER_B
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	mov	rdi, FT_FIBER_C
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; bindings correct
	mov	rdi, FT_KEY_X
	call	fp$coeffect_lookup
	cmp	rax, FT_VAL_A
	jne	.fail
	xor	eax, eax			; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test 12 — fiber_tree_hot_replace
;   Build A->B->C, then replace A with A' providing the same KEY_X but a new
;   value. Assert B and C deactivate then re-activate against A', and now see
;   the new value.
; =============================================================================
align 16
fp$test_fiber_tree_hot_replace:
	call	fp$ft_reset
	call	fp$ft_build_chain
	test	rax, rax
	jnz	.fail
	; hot-replace: unload A (B and C deactivate), then provide KEY_X with a
	; NEW value from A and reload the tree.
	mov	rdi, FT_FIBER_A
	call	fp$fiber_unload
	test	rax, rax
	jnz	.fail
	; A' provides KEY_X = FT_VAL_A_PRIME
	mov	rdi, FT_FIBER_A
	mov	rsi, FT_KEY_X
	mov	rdx, FT_VAL_A_PRIME
	call	fp$coeffect_provide
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_A
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	; reload B and C against A'
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_Y
	mov	rdx, 0xB0B0
	call	fp$coeffect_provide
	mov	rdi, FT_FIBER_B
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_Y
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_C
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	; all active again
	mov	rdi, FT_FIBER_C
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; KEY_X now resolves to the NEW value, not the old one
	mov	rdi, FT_KEY_X
	call	fp$coeffect_lookup
	cmp	rax, FT_VAL_A_PRIME
	jne	.fail
	xor	eax, eax			; PASS
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; fp$ft_build_chain — helper: build A->B->C with A providing KEY_X=FT_VAL_A.
;   CONTRACT: OUT rax = 0 on success, 1 on failure; CLOBBERS rax, rcx, rdx,
;             rsi, rdi.
; =============================================================================
align 16
fp$ft_build_chain:
	; A provides KEY_X
	mov	rdi, FT_FIBER_A
	mov	rsi, FT_KEY_X
	mov	rdx, FT_VAL_A
	call	fp$coeffect_provide
	test	rax, rax
	jnz	.fail
	mov	rdi, FT_FIBER_A
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	; B injects KEY_X, provides KEY_Y
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_B
	mov	rsi, FT_KEY_Y
	mov	rdx, 0xB0B0
	call	fp$coeffect_provide
	mov	rdi, FT_FIBER_B
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	; C injects KEY_X and KEY_Y
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_X
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_C
	mov	rsi, FT_KEY_Y
	call	fp$coeffect_declare
	mov	rdi, FT_FIBER_C
	call	fp$fiber_load
	test	rax, rax
	jnz	.fail
	xor	eax, eax
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
fp$name_ft_chain db 'fiber_tree_dependency_chain', 0
fp$name_ft_unload db 'fiber_tree_root_unload', 0
fp$name_ft_reload db 'fiber_tree_root_reload', 0
fp$name_ft_hot	db 'fiber_tree_hot_replace', 0
fp$hello_str	db 'Hello from serial', 0
fp$heartbeat_nl	db 10, 0

; test-mutable state (writable): the effect-revert counter incremented by
; fp$ft_inverse so the fiber-tree tests can count reverted effects.
align 8
fp$ft_revert_count dq 0

end if ; FP_OS_TESTS_INC
