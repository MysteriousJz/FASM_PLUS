; =============================================================================
; FASM_PLUS OS Core — IDT.INC
; Interrupt Descriptor Table: routes exceptions and interrupts to handlers.
;
; Depends on: GDT.INC (for the kernel code selector), SERIAL.INC
; Emits: functions in the current code section; the IDTR lives in the current
;        data area. The 256-entry IDT itself is a BSS region allocated by
;        OS_TEST.ASM as fp$idt_image (4 KB, 16-byte aligned).
;
; 64-bit IDT gate descriptor format (16 bytes):
;   bytes 0-1   offset 15:0        (handler address)
;   bytes 2-3   selector           (kernel code segment)
;   byte  4     IST                (0 = no interrupt stack table)
;   byte  5     type/attributes    (P DPL 0 1 1 1 0  -> 0x8E for ring0 int gate)
;   bytes 6-7   offset 31:16
;   bytes 8-11  offset 63:32
;   bytes 12-15 reserved (0)
;
; An interrupt gate (type 0xE) clears IF on entry, so the handler runs with
; interrupts masked — required for exception handlers that must not be
; re-entered before they halt the machine.
; =============================================================================

if ~ definite FP_IDT_INC
FP_IDT_INC = 1

if ~ definite FP_GDT_INC
  err 'IDT.INC requires GDT.INC (and CONSTANTS.INC, SERIAL.INC) first'
end if

IDT_ENTRIES         = 256
IDT_ENTRY_SIZE      = 16
IDT_GATE_INT        = 0x8E        ; present, ring 0, 64-bit interrupt gate
IDT_GATE_TRAP       = 0x8F        ; present, ring 0, 64-bit trap gate

; =============================================================================
; fp$idt_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none
;   Zeroes all 256 entries and loads the IDT with lidt. Until
;   fp$idt_set_entry installs a handler, every vector is unmapped (a trigger
;   would double-fault, which is the correct controlled-halt behavior).
; =============================================================================
align 16
fp$idt_init:
	mov	rcx, fp$idt_image
	mov	edx, IDT_ENTRIES * 2	; 256 entries x 2 qwords each
      .clear:
	xor	eax, eax
	mov	[rcx], rax
	add	rcx, 8
	dec	edx
	jnz	.clear
	lidt	[fp$idtr]
	ret

; =============================================================================
; fp$idt_set_entry
;   CONTRACT
;     IN        : rdi = vector number (0-255), rsi = handler address
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none (vector is masked to 0-255)
;   Sets an IDT entry as a present ring-0 interrupt gate pointing at the
;   handler. The descriptor is assembled field-by-field; each field write is
;   a single obvious shift, keeping the generated code transparent.
; =============================================================================
align 16
fp$idt_set_entry:
	and	edi, 0xFF		; clamp vector to 0-255
	shl	edi, 4			; x 16 bytes per entry (no multiply)
	mov	rcx, fp$idt_image
	add	rcx, rdi		; rcx = &IDT[vector]
	; bytes 0-1: offset 15:0
	mov	rax, rsi
	mov	[rcx], ax
	; bytes 2-3: selector = kernel code
	mov	word [rcx + 2], GDT_KERNEL_CODE
	; byte 4: IST = 0 (no alternate stack)
	mov	byte [rcx + 4], 0
	; byte 5: type = present ring0 interrupt gate
	mov	byte [rcx + 5], IDT_GATE_INT
	; bytes 6-7: offset 31:16
	mov	rax, rsi
	shr	rax, 16
	mov	[rcx + 6], ax
	; bytes 8-11: offset 63:32
	mov	rax, rsi
	shr	rax, 32
	mov	[rcx + 8], eax
	; bytes 12-15: reserved = 0
	xor	eax, eax
	mov	[rcx + 12], eax
	ret

; =============================================================================
; fp$idt_get_entry
;   CONTRACT
;     IN        : rdi = vector number (0-255)
;     OUT       : rax = handler address (0 if unmapped)
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none
;   Reads back the handler offset stored in an IDT entry. Used by the IDT
;   self-test to confirm a handler was installed correctly.
; =============================================================================
align 16
fp$idt_get_entry:
	and	edi, 0xFF
	shl	edi, 4
	mov	rcx, fp$idt_image
	add	rcx, rdi
	movzx	eax, word [rcx]		; offset 15:0
	movzx	edx, word [rcx + 6]	; offset 31:16
	shl	rdx, 16
	or	rax, rdx
	mov	edx, dword [rcx + 8]	; offset 63:32
	shl	rdx, 32
	or	rax, rdx
	ret

; -----------------------------------------------------------------------------
; Read-only data: the IDTR (limit, base). The IDT image is the BSS region
; fp$idt_image allocated by OS_TEST.ASM.
; -----------------------------------------------------------------------------
align 16
fp$idtr:
	dw IDT_ENTRIES * IDT_ENTRY_SIZE - 1	; limit = 4095
	dq fp$idt_image				; base

end if ; FP_IDT_INC
