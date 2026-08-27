; =============================================================================
; FASM_PLUS T440 Layer — PS2_KEYBOARD.INC
; PS/2 keyboard driver: read scancodes from the 8042 controller and translate
; them to ASCII, buffered for the kernel.
;
; Depends on: CORDIS.INC (this driver is a Cordis fiber), IDT.INC (IRQ1),
;             KERNEL_INIT.INC (the PIC is already remapped; IRQ1 = vector 33)
; Emits: functions + the IRQ1 handler in the code section; the scancode
;        buffer and controller state live in BSS.
;
; Cordis integration
; ------------------
;   Fiber name : PS2_KEYBOARD (fiber id T440_FIBER_KBD)
;   Provides   : KEYBOARD_INPUT (value = base of the character ring buffer)
;   Injects    : IRQ1_HANDLER (the IDT routes IRQ1; this fiber installs the
;                handler body into vector 33)
;   Effects    : enabling the keyboard and unmasking IRQ1 are revertible
;                (inverse: mask IRQ1, disable the keyboard scan).
;
; Hardware (Intel 8042 / HT6542B; see HT6542B.PDF and the OSDev 8042 page)
; ------------------------------------------------------------------------
;   Port 0x60  data  (read: scancode/output; write: keyboard command byte)
;   Port 0x64  status(read)/command(write)
;     status bit 0 (0x01) = output buffer full (a scancode is waiting)
;     status bit 1 (0x02) = input buffer full (controller busy)
;   Controller commands (write 0x64): 0xAE enable keyboard, 0xAD disable,
;     0x20 read config byte, 0x60 write config byte
;   Keyboard commands (write 0x60): 0xF0 = get/set scancode set (arg follows)
;   PIC: IRQ1 is vector 33 after the remap; it must be unmasked in PIC1 OCW1.
;
; Scancode set 2 (make codes, single byte) is translated to ASCII with a
; fixed 128-entry lookup table (no division). Break codes (0xF0 prefix) and
; the extended prefix (0xE0) are consumed and ignored for now — the bootstrap
; echoes key presses, not releases.
; =============================================================================

if ~ definite FP_PS2_KEYBOARD_INC
FP_PS2_KEYBOARD_INC = 1

if ~ definite FP_IDT_INC
  err 'PS2_KEYBOARD.INC requires IDT.INC (and CORDIS.INC) first'
end if

; -----------------------------------------------------------------------------
; 8042 ports, status bits, and commands
; -----------------------------------------------------------------------------
PS2_DATA_PORT    = 0x60
PS2_STATUS_PORT  = 0x64
PS2_COMMAND_PORT = 0x64

PS2_STATUS_OBF   = 0x01          ; output buffer full
PS2_STATUS_IBF   = 0x02          ; input buffer full

PS2_CMD_READCFG  = 0x20          ; read controller configuration byte
PS2_CMD_WRITECFG = 0x60          ; write controller configuration byte
PS2_CMD_ENABLE_KBD = 0xAE        ; enable first PS/2 port (keyboard)
PS2_CMD_DISABLE_KBD = 0xAD       ; disable first PS/2 port

KBD_CMD_SCANCODE = 0xF0          ; get/set scancode set
KBD_SCANCODE_SET2 = 0x02
KBD_CMD_ENABLE   = 0xF4          ; enable scanning

; IRQ1 -> IDT vector (PIC remap puts IRQ0 at 32, so IRQ1 = 33)
PS2_IRQ_VECTOR   = 33
; PIC1 data port (OCW1 mask register)
PIC1_DATA        = 0x21
PIC1_COMMAND     = 0x20
PIC_EOI          = 0x20
; IRQ1 mask bit in PIC1 (bit 1)
PIC_IRQ1_MASK    = 0x02

; ring buffer capacity (power of two so wrap is an AND, no division)
KBD_BUF_SIZE     = 256
KBD_BUF_MASK     = KBD_BUF_SIZE - 1

; Coeffect key this fiber provides; value = ring buffer base.
COEFFECT_KEYBOARD_INPUT = 0x7003
; Coeffect key this fiber injects (the IDT's IRQ1 routing provision). The IDT
; is set up by KERNEL_INIT, which we treat as always-provided at this layer.
COEFFECT_IRQ1_HANDLER = 0x7004

; =============================================================================
; fp$ps2_wait_write — wait until the controller's input buffer is empty
;   CONTRACT: CLOBBERS rax, rdx, rcx, flags. ERRORS: none (bounded spin).
; =============================================================================
align 16
fp$ps2_wait_write:
	mov	rcx, 0x100000		; bounded spin (no infinite hang on HW)
	mov	rdx, PS2_STATUS_PORT
      .wait:
	in	al, dx
	test	al, PS2_STATUS_IBF
	jz	.ready
	dec	rcx
	jnz	.wait
      .ready:
	ret

; =============================================================================
; fp$ps2_wait_read — wait until the output buffer has a byte
;   CONTRACT: OUT rax = 0 if a byte is ready, -1 on timeout.
;             CLOBBERS rax, rdx, rcx, flags. ERRORS: -1 on timeout.
; =============================================================================
align 16
fp$ps2_wait_read:
	mov	rcx, 0x100000
	mov	rdx, PS2_STATUS_PORT
      .wait:
	in	al, dx
	test	al, PS2_STATUS_OBF
	jnz	.ready
	dec	rcx
	jnz	.wait
	mov	rax, -1			; timeout
	ret
      .ready:
	xor	eax, eax
	ret

; =============================================================================
; fp$kbd_irq_handler — IRQ1 interrupt handler (vector 33)
;   Reads the scancode from port 0x60, translates a make-code to ASCII, pushes
;   it onto the ring buffer, then sends EOI to the PIC. Runs in interrupt
;   context; saves every register it touches.
; =============================================================================
align 16
fp$kbd_irq_handler:
	push	rax
	push	rcx
	push	rdx
	push	rdi
	push	rsi
	; read the scancode
	mov	rdx, PS2_DATA_PORT
	in	al, dx			; al = scancode
	movzx	esi, al
	; ignore break-code prefix (0xF0) and extended prefix (0xE0): remember
	; the next scancode is a break/extended and skip translation.
	cmp	al, 0xF0
	je	.mark_break
	cmp	al, 0xE0
	je	.mark_ext
	; if the previous byte was a break prefix, swallow this code (release)
	cmp	byte [fp$kbd_break_pending], 0
	jne	.swallow
	; translate make code -> ASCII
	lea	rcx, [fp$kbd_scancode_table]
	movzx	eax, byte [rcx + rsi]
	test	al, al
	jz	.done			; untranslatable -> ignore
	; push onto the ring buffer (drop on full)
	mov	ecx, [fp$kbd_head]
	mov	edi, [fp$kbd_tail]
	lea	esi, [rcx + 1]
	and	esi, KBD_BUF_MASK
	cmp	esi, edi		; full?
	je	.done
	lea	rdx, [fp$kbd_buffer]
	mov	[rdx + rcx], al		; store ASCII
	mov	[fp$kbd_head], esi
	jmp	.done
      .mark_break:
	mov	byte [fp$kbd_break_pending], 1
	jmp	.done
      .mark_ext:
	; extended scancode: treat like a break boundary for simplicity
	mov	byte [fp$kbd_break_pending], 1
	jmp	.done
      .swallow:
	mov	byte [fp$kbd_break_pending], 0
      .done:
	; EOI to PIC1 so the next IRQ1 can fire
	mov	rdx, PIC1_COMMAND
	mov	al, PIC_EOI
	out	dx, al
	pop	rsi
	pop	rdi
	pop	rdx
	pop	rcx
	pop	rax
	iretq

; =============================================================================
; fp$kbd_getchar — pop a character from the ring buffer
;   CONTRACT
;     IN        : nothing
;     OUT       : rax = ASCII character, or -1 if the buffer is empty
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, rsi, rdi, r8-r15
;     ERRORS    : -1 when empty
; =============================================================================
align 16
fp$kbd_getchar:
	mov	ecx, [fp$kbd_tail]
	cmp	ecx, [fp$kbd_head]
	je	.empty
	lea	rdx, [fp$kbd_buffer]
	movzx	eax, byte [rdx + rcx]
	inc	ecx
	and	ecx, KBD_BUF_MASK
	mov	[fp$kbd_tail], ecx
	ret
      .empty:
	mov	rax, -1
	ret

; =============================================================================
; fp$ps2_kbd_apply — the Cordis fiber's apply function
;   CONTRACT
;     IN        : nothing (fiber id is the constant T440_FIBER_KBD)
;     OUT       : rax = 0 on success
;     CLOBBERS  : all
;     ERRORS    : -1 if the controller never becomes ready (no PS/2 present)
;   Initializes the 8042, enables the keyboard, installs the IRQ1 handler into
;   IDT vector 33, and unmasks IRQ1 in the PIC. The enable + unmask are
;   recorded as a revertible effect (inverse masks IRQ1 and disables scanning).
; =============================================================================
align 16
fp$ps2_kbd_apply:
	; record the revertible effect first (LIFO keeps ordering safe)
	mov	rdi, T440_FIBER_KBD
	mov	rsi, fp$ps2_kbd_inverse
	xor	rdx, rdx
	call	fp$effect_record
	test	rax, rax
	js	.fail
	; disable the keyboard while we configure it
	call	fp$ps2_wait_write
	mov	rdx, PS2_COMMAND_PORT
	mov	al, PS2_CMD_DISABLE_KBD
	out	dx, al
	; flush any pending output byte
	mov	rdx, PS2_DATA_PORT
	in	al, dx
	; read the controller configuration byte
	call	fp$ps2_wait_write
	mov	rdx, PS2_COMMAND_PORT
	mov	al, PS2_CMD_READCFG
	out	dx, al
	call	fp$ps2_wait_read
	test	rax, rax
	jnz	.fail
	mov	rdx, PS2_DATA_PORT
	in	al, dx			; al = config byte
	; set bit 0 (enable IRQ1) and clear bit 4 (enable keyboard clock)
	or	al, 0x01
	and	al, 0xEF		; clear bit 4 (0 = keyboard clock enabled)
	mov	cl, al			; save new config
	call	fp$ps2_wait_write
	mov	rdx, PS2_COMMAND_PORT
	mov	al, PS2_CMD_WRITECFG
	out	dx, al
	call	fp$ps2_wait_write
	mov	rdx, PS2_DATA_PORT
	mov	al, cl
	out	dx, al
	; select scancode set 2
	call	fp$ps2_wait_write
	mov	rdx, PS2_DATA_PORT
	mov	al, KBD_CMD_SCANCODE
	out	dx, al
	call	fp$ps2_wait_write
	mov	rdx, PS2_DATA_PORT
	mov	al, KBD_SCANCODE_SET2
	out	dx, al
	; enable the keyboard port and scanning
	call	fp$ps2_wait_write
	mov	rdx, PS2_COMMAND_PORT
	mov	al, PS2_CMD_ENABLE_KBD
	out	dx, al
	call	fp$ps2_wait_write
	mov	rdx, PS2_DATA_PORT
	mov	al, KBD_CMD_ENABLE
	out	dx, al
	; install the IRQ1 handler into the IDT (vector 33)
	mov	rdi, PS2_IRQ_VECTOR
	mov	rsi, fp$kbd_irq_handler
	call	fp$idt_set_entry
	; unmask IRQ1 in PIC1 (clear bit 1 of OCW1)
	mov	rdx, PIC1_DATA
	in	al, dx
	and	al, not PIC_IRQ1_MASK
	out	dx, al
	; provide the input buffer to dependents
	mov	rdi, T440_FIBER_KBD
	mov	rsi, COEFFECT_KEYBOARD_INPUT
	mov	rdx, fp$kbd_buffer
	call	fp$coeffect_provide
	test	rax, rax
	js	.fail
	xor	eax, eax
	ret
      .fail:
	mov	rax, -1
	ret

; fp$ps2_kbd_inverse — revert the keyboard effect (mask IRQ1, disable scanning)
;   CONTRACT: IN rdi = context (ignored); CLOBBERS rax, rdx, flags.
align 16
fp$ps2_kbd_inverse:
	; mask IRQ1 in PIC1
	mov	rdx, PIC1_DATA
	in	al, dx
	or	al, PIC_IRQ1_MASK
	out	dx, al
	; disable keyboard scanning (best-effort; do not block on a dead ctrl)
	mov	rdx, PS2_COMMAND_PORT
	mov	al, PS2_CMD_DISABLE_KBD
	out	dx, al
	ret

; =============================================================================
; Scancode set 2 -> ASCII translation table (make codes 0..127).
; 0 = untranslatable. Only the common printable keys are mapped; this is the
; bootstrap echo path, not a full IME.
; =============================================================================
align 16
fp$kbd_scancode_table:
	; index = scancode. Filled below (128 bytes).
	db 0		; 0x00
	db 0		; 0x01 F9
	db 0		; 0x02
	db 0		; 0x03 F5
	db 0		; 0x04 F3
	db 0		; 0x05 F1
	db 0		; 0x06 F2
	db 0		; 0x07 F12
	db 0		; 0x08
	db 0		; 0x09 F10
	db 0		; 0x0A F8
	db 0		; 0x0B F6
	db 0		; 0x0C F4
	db 9		; 0x0D Tab
	db '`'		; 0x0E `
	db 0		; 0x0F
	db 0		; 0x10
	db 0		; 0x11 LAlt
	db 0		; 0x12 LShift
	db 0		; 0x13
	db 0		; 0x14 LCtrl
	db 'q'		; 0x15
	db '1'		; 0x16
	db 0		; 0x17
	db 0		; 0x18
	db 0		; 0x19
	db 'z'		; 0x1A
	db 's'		; 0x1B
	db 'a'		; 0x1C
	db 'w'		; 0x1D
	db '2'		; 0x1E
	db 0		; 0x1F
	db 0		; 0x20
	db 'c'		; 0x21
	db 'x'		; 0x22
	db 'd'		; 0x23
	db 'e'		; 0x24
	db '4'		; 0x25
	db '3'		; 0x26
	db 0		; 0x27
	db 0		; 0x28
	db ' '		; 0x29 space
	db 'v'		; 0x2A
	db 'f'		; 0x2B
	db 't'		; 0x2C
	db 'r'		; 0x2D
	db '5'		; 0x2E
	db 0		; 0x2F
	db 0		; 0x30
	db 'n'		; 0x31
	db 'b'		; 0x32
	db 'h'		; 0x33
	db 'g'		; 0x34
	db 'y'		; 0x35
	db '6'		; 0x36
	db 0		; 0x37
	db 0		; 0x38
	db 0		; 0x39
	db 'm'		; 0x3A
	db 'j'		; 0x3B
	db 'u'		; 0x3C
	db '7'		; 0x3D
	db '8'		; 0x3E
	db 0		; 0x3F
	db 0		; 0x40
	db ','		; 0x41
	db 'k'		; 0x42
	db 'i'		; 0x43
	db 'o'		; 0x44
	db '0'		; 0x45
	db '9'		; 0x46
	db 0		; 0x47
	db 0		; 0x48
	db '.'		; 0x49
	db '/'		; 0x4A
	db 'l'		; 0x4B
	db ';'		; 0x4C
	db 'p'		; 0x4D
	db '-'		; 0x4E
	db 0		; 0x4F
	db 0		; 0x50
	db 0		; 0x51
	db 0x27		; 0x52 '
	db 0		; 0x53
	db '['		; 0x54
	db '='		; 0x55
	db 0		; 0x56
	db 0		; 0x57
	db 0		; 0x58 CapsLock
	db 0		; 0x59 RShift
	db 13		; 0x5A Enter
	db ']'		; 0x5B
	db 0		; 0x5C
	db 0x5C		; 0x5D backslash
	db 0		; 0x5E
	db 0		; 0x5F
	db 0		; 0x60
	db 0		; 0x61
	db 0		; 0x62
	db 0		; 0x63
	db 0		; 0x64
	db 0		; 0x65
	db 8		; 0x66 Backspace
	db 0		; 0x67
	db 0		; 0x68
	db 0		; 0x69 KP1
	db 0		; 0x6A
	db 0		; 0x6B KP4
	db 0		; 0x6C KP7
	db 0		; 0x6D
	db 0		; 0x6E
	db 0		; 0x6F
	db 0		; 0x70 KP0
	db 0		; 0x71 KP.
	db 0		; 0x72 KP2
	db 0		; 0x73 KP5
	db 0		; 0x74 KP6
	db 0		; 0x75 KP8
	db 27		; 0x76 Esc
	db 0		; 0x77 NumLock
	db 0		; 0x78 F11
	db 0		; 0x79 KP+
	db 0		; 0x7A KP3
	db 0		; 0x7B KP-
	db 0		; 0x7C KP*
	db 0		; 0x7D KP9
	db 0		; 0x7E ScrollLock
	db 0		; 0x7F
; (indices 0x00-0x7F above are exactly 128 entries — the table is full)

end if ; FP_PS2_KEYBOARD_INC
