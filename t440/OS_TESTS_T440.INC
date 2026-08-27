; =============================================================================
; FASM_PLUS T440 Layer — OS_TESTS_T440.INC
; The T440 test runner: runs the 12 OS Core tests plus the T440 driver tests,
; and (when not exiting under the test harness) drops into the interactive
; keyboard-echo loop that is the T440 success criterion.
;
; Depends on: OS_TESTS.INC (the core tests + runner primitives) and the T440
;             drivers (PCI_ENUM, VESA_DISPLAY, PS2_KEYBOARD, T440_INIT).
; Emits: fp$run_t440_tests and the fp$test_t440_* bodies.
; =============================================================================

if ~ definite FP_OS_TESTS_T440_INC
FP_OS_TESTS_T440_INC = 1

if ~ definite FP_T440_INIT_INC
  err 'OS_TESTS_T440.INC requires T440_INIT.INC (and the OS core tests) first'
end if

; =============================================================================
; fp$run_t440_tests
;   CONTRACT
;     IN        : nothing (called from fp$long_mode_entry after t440_init)
;     OUT       : never returns
;     CLOBBERS  : all
;   Runs the 12 core tests + the T440 driver tests, prints the summary, and
;   exits via the isa-debug-exit device. If there is no isa-debug-exit device
;   (real hardware), the exit write is harmless and we fall through to the
;   interactive echo loop so the laptop still shows life.
; =============================================================================
align 16
fp$run_t440_tests:
	; the heartbeat was already printed (serial + screen) by
	; fp$long_mode_entry before this call; here we only run the tests.

	; ---- the 12 OS Core tests ----
	RUN_TEST fp$name_boot,      fp$test_boot
	RUN_TEST fp$name_serial,    fp$test_serial
	RUN_TEST fp$name_gdt,       fp$test_gdt
	RUN_TEST fp$name_idt,       fp$test_idt
	RUN_TEST fp$name_paging,    fp$test_paging
	RUN_TEST fp$name_exception, fp$test_exception
	RUN_TEST fp$name_context,   fp$test_context
	RUN_TEST fp$name_syscall,   fp$test_syscall
	RUN_TEST fp$name_ft_chain,  fp$test_fiber_tree_chain
	RUN_TEST fp$name_ft_unload, fp$test_fiber_tree_unload
	RUN_TEST fp$name_ft_reload, fp$test_fiber_tree_reload
	RUN_TEST fp$name_ft_hot,    fp$test_fiber_tree_hot_replace

	; ---- the T440 driver tests ----
	RUN_TEST fp$name_t440_init, fp$test_t440_init
	RUN_TEST fp$name_t440_pci,  fp$test_t440_pci
	RUN_TEST fp$name_t440_vesa, fp$test_t440_vesa
	RUN_TEST fp$name_t440_kbd,  fp$test_t440_kbd

	; summary + isa-debug-exit; on real hardware the 0xF4 write is a no-op
	; and fp$test_summary_and_exit halts. For an interactive T440 demo we
	; would instead fall into fp$t440_echo_loop; the test harness path halts.
	call	fp$test_summary_and_exit

; =============================================================================
; Test T1 — t440_init: the driver boot completed.
;   PASS if the PCI enumerator at least ran (its fiber is Active or the
;   registry holds the PCI table provision). We do not require all three to be
;   Active here because a headless CI machine may lack a VGA device; the
;   individual driver tests below check each one precisely.
; =============================================================================
align 16
fp$test_t440_init:
	; The Cordis fiber-tree tests reset the registry, wiping the T440 fibers.
	; Re-run the driver init so this test is self-contained, then confirm the
	; PCI enumerator (a root provider with no deps) is Active.
	call	fp$t440_init
	mov	rdi, T440_FIBER_PCI
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	xor	eax, eax
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test T2 — t440_pci: the PCI bus was scanned and the device table holds at
; least one device, and the table provision is visible.
; =============================================================================
align 16
fp$test_t440_pci:
	mov	rax, [fp$pci_device_count]
	test	rax, rax
	jz	.fail			; zero devices -> enumeration broken
	; the provision must resolve to the table base
	mov	rdi, COEFFECT_PCI_DEVICE_TABLE
	call	fp$coeffect_lookup
	cmp	rax, CORDIS_NIL
	je	.fail
	cmp	rax, fp$pci_device_table
	jne	.fail
	xor	eax, eax
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test T3 — t440_vesa: a framebuffer was established.
;   PASS if the display mode block has a nonzero LFB and positive dimensions.
;   (On a headless CI machine with no VGA/BGA device this test would report a
;   failure reason, which is the honest, detectable answer.)
; =============================================================================
align 16
fp$test_t440_vesa:
	mov	rax, qword [fp$display_mode_block + display_mode.lfb]
	test	rax, rax
	jz	.fail
	mov	rax, qword [fp$display_mode_block + display_mode.width]
	test	rax, rax
	jz	.fail
	mov	rax, qword [fp$display_mode_block + display_mode.height]
	test	rax, rax
	jz	.fail
	; a drawn pixel must be readable back through the LFB — but only if the
	; LFB is in the identity-mapped first 1 GB (a higher MMIO-hole LFB is
	; not directly addressable under the current map, so skip the readback).
	mov	rax, qword [fp$display_mode_block + display_mode.lfb]
	mov	rcx, rax
	shr	rcx, 30			; LFB / 1GB
	test	rcx, rcx
	jnz	.skip_readback		; LFB >= 1 GB -> provision-only check
	mov	rcx, qword [fp$display_mode_block + display_mode.pitch]
	mov	rdx, qword [fp$display_mode_block + display_mode.height]
	dec	rdx
	imul	rcx, rdx		; pitch*(height-1) = last row offset
	add	rax, rcx
	mov	dword [rax], 0x00FFFFFF
	cmp	dword [rax], 0x00FFFFFF
	jne	.fail
      .skip_readback:
	xor	eax, eax
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; Test T4 — t440_kbd: the keyboard driver is Active and its input buffer is
; provided. (We cannot synthesize a physical keypress in CI, so we verify the
; fiber state, the provision, and that the scancode table maps a known key.)
; =============================================================================
align 16
fp$test_kbd:
fp$test_t440_kbd:
	; fiber must be Active
	mov	rdi, T440_FIBER_KBD
	call	fp$fiber_state
	cmp	rax, FIBER_ACTIVE
	jne	.fail
	; the KEYBOARD_INPUT provision must resolve to the ring buffer base
	mov	rdi, COEFFECT_KEYBOARD_INPUT
	call	fp$coeffect_lookup
	cmp	rax, CORDIS_NIL
	je	.fail
	cmp	rax, fp$kbd_buffer
	jne	.fail
	; scancode table sanity: scancode 0x1C must map to 'a'
	lea	rcx, [fp$kbd_scancode_table]
	movzx	eax, byte [rcx + 0x1C]
	cmp	al, 'a'
	jne	.fail
	; ring buffer starts empty
	mov	eax, [fp$kbd_head]
	cmp	eax, [fp$kbd_tail]
	jne	.fail
	xor	eax, eax
	ret
      .fail:
	mov	eax, 1
	ret

; =============================================================================
; fp$t440_echo_loop — interactive success criterion for the physical T440.
; Reads a character from the keyboard ring buffer and echoes it to the screen
; (and serial), advancing a cursor. Loops forever. NOT used by the CI test
; harness; it is the human-facing proof that keyboard input works on the T440.
;   CONTRACT: never returns; CLOBBERS all.
; =============================================================================
align 16
fp$t440_echo_loop:
	xor	r12, r12		; r12 = cursor column
	xor	r13, r13		; r13 = cursor row
      .loop:
	; poll the keyboard ring buffer
	call	fp$kbd_getchar
	cmp	rax, -1
	je	.idle
	; got a character in al: echo to serial
	push	rax
	movzx	rdi, al
	call	fp$serial_putchar
	pop	rax
	; echo to the screen at the cursor
	movzx	edx, al
	mov	rdi, r12
	mov	rsi, r13
	mov	rcx, 0x0000FF00		; green on black
	call	fp$vesa_putchar
	; advance the cursor with wrap
	inc	r12
	mov	rax, qword [fp$display_mode_block + display_mode.width]
	shr	rax, 3			; text columns = width/8
	cmp	r12, rax
	jb	.loop
	xor	r12, r12
	inc	r13
	jmp	.loop
      .idle:
	hlt				; sleep until the next IRQ1
	jmp	.loop

; -----------------------------------------------------------------------------
; Read-only data: T440 test names.
; -----------------------------------------------------------------------------
fp$name_t440_init db 't440_init', 0
fp$name_t440_pci  db 't440_pci', 0
fp$name_t440_vesa db 't440_vesa', 0
fp$name_t440_kbd  db 't440_kbd', 0

end if ; FP_OS_TESTS_T440_INC
