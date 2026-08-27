; =============================================================================
; FASM_PLUS T440 Layer — T440_INIT.INC
; T440 boot sequence: loads the three hardware drivers as Cordis fibers in
; dependency order.
;
; Depends on: PCI_ENUM.INC, VESA_DISPLAY.INC, PS2_KEYBOARD.INC, CORDIS.INC
; Emits: fp$t440_init in the current code section.
;
; Load order (dependency order)
; -----------------------------
;   1. PCI_ENUMERATOR  — root provider; nothing to inject
;   2. VESA_DISPLAY    — injects PCI_DEVICE_TABLE, provides DISPLAY_FRAMEBUFFER
;   3. PS2_KEYBOARD    — injects IRQ1_HANDLER, provides KEYBOARD_INPUT
;
; Each driver is loaded as a Cordis fiber:
;   effect_begin(fiber)            open an effect scope
;   <declare injects>              declare what the driver needs
;   <apply>                        install the driver (records its effects)
;   effect_commit(fiber)           keep the effects
;   fiber_load(fiber)              activate once dependencies resolve
;
; If a driver's dependencies are not yet satisfied, fiber_load leaves it in
; Loading; loading the provider afterward activates it. Because we load in
; dependency order, each driver activates immediately.
; =============================================================================

if ~ definite FP_T440_INIT_INC
FP_T440_INIT_INC = 1

if ~ definite FP_PS2_KEYBOARD_INC
  err 'T440_INIT.INC requires PS2_KEYBOARD.INC (and PCI_ENUM, VESA_DISPLAY) first'
end if

; -----------------------------------------------------------------------------
; T440 fiber ids (registry slots). 0 is the bootstrap/idle slot; the drivers
; use 10..12 to stay clear of the Cordis self-test slots (1..3).
; -----------------------------------------------------------------------------
T440_FIBER_PCI  = 10
T440_FIBER_VESA = 11
T440_FIBER_KBD  = 12

; =============================================================================
; fp$t440_init — load the T440 hardware drivers in dependency order
;   CONTRACT
;     IN        : nothing (called from the kernel after fp$kernel_init)
;     OUT       : rax = 0 if all three drivers activated, else a bitmap of
;                 failures (bit0=PCI, bit1=VESA, bit2=KBD)
;     CLOBBERS  : all
;     ERRORS    : per-driver failure folded into the return bitmap
; =============================================================================
align 16
fp$t440_init:
	push	rbx
	xor	ebx, ebx		; rbx = failure bitmap

	; ---------------------------------------------------------------------
	; Driver 1: PCI_ENUMERATOR (root provider)
	; ---------------------------------------------------------------------
	mov	rdi, T440_FIBER_PCI
	call	fp$effect_begin
	call	fp$pci_enum_apply
	test	rax, rax
	jnz	.pci_fail
	mov	rdi, T440_FIBER_PCI
	call	fp$effect_commit
	mov	rdi, T440_FIBER_PCI
	call	fp$fiber_load
	test	rax, rax
	jnz	.pci_fail
	jmp	.pci_done
      .pci_fail:
	or	ebx, 0x01
      .pci_done:

	; ---------------------------------------------------------------------
	; Driver 2: VESA_DISPLAY (injects PCI_DEVICE_TABLE)
	; ---------------------------------------------------------------------
	; ---------------------------------------------------------------------
	; Driver 2: VESA_DISPLAY (injects PCI_DEVICE_TABLE)
	; ---------------------------------------------------------------------
	mov	rdi, T440_FIBER_VESA
	call	fp$effect_begin
	mov	rdi, T440_FIBER_VESA
	mov	rsi, COEFFECT_PCI_DEVICE_TABLE
	call	fp$coeffect_declare
	call	fp$vesa_apply
	test	rax, rax
	jnz	.vesa_fail
	mov	rdi, T440_FIBER_VESA
	call	fp$effect_commit
	mov	rdi, T440_FIBER_VESA
	call	fp$fiber_load
	test	rax, rax
	jnz	.vesa_fail
	jmp	.vesa_done
      .vesa_fail:
	or	ebx, 0x02
      .vesa_done:

	; ---------------------------------------------------------------------
	; Driver 3: PS2_KEYBOARD (injects IRQ1_HANDLER)
	; ---------------------------------------------------------------------
	mov	rdi, T440_FIBER_KBD
	call	fp$effect_begin
	mov	rdi, T440_FIBER_KBD
	mov	rsi, COEFFECT_IRQ1_HANDLER
	call	fp$coeffect_declare
	call	fp$ps2_kbd_apply
	test	rax, rax
	jnz	.kbd_fail
	mov	rdi, T440_FIBER_KBD
	call	fp$effect_commit
	; publish IRQ1_HANDLER as provided by the bootstrap fiber (slot 0) so
	; the keyboard's dependency resolves and it can activate.
	mov	rdi, 0
	mov	rsi, COEFFECT_IRQ1_HANDLER
	mov	rdx, fp$kbd_irq_handler
	call	fp$coeffect_provide
	mov	rdi, T440_FIBER_KBD
	call	fp$fiber_load
	test	rax, rax
	jnz	.kbd_fail
	jmp	.kbd_done
      .kbd_fail:
	or	ebx, 0x04
      .kbd_done:

	mov	rax, rbx
	pop	rbx
	ret

end if ; FP_T440_INIT_INC
