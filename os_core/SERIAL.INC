; =============================================================================
; FASM_PLUS OS Core — SERIAL.INC
; 16550 UART serial driver for COM1 (I/O port 0x3F8) at 115200 baud, 8N1.
;
; Depends on: nothing (bare metal; uses only in/out instructions)
; Emits: functions in the current code section; read-only data for the
;        hex prefix and the decimal power-of-ten table.
;
; Philosophy
; ----------
; Serial is the kernel's only reliable output channel before (and after)
; paging, the GDT, or the IDT exist. Every other component reports through
; here, so this file has the fewest dependencies in the OS core: it must be
; correct before anything else can be debugged.
;
; The port protocol is the 16550 UART register set at base 0x3F8:
;   +0  RBR/THR (read: receive buffer, write: transmit holding)
;   +1  IER/DLM (interrupt enable / divisor latch high, DLAB-dependent)
;   +2  IIR/FCR (interrupt identification / FIFO control)
;   +3  LCR     (line control; bit 7 = DLAB)
;   +4  MCR     (modem control)
;   +5  LSR     (line status; bit 5 = THR empty)
;
; No division is used anywhere. Decimal formatting uses repeated subtraction
; of powers of ten, which honors the Mechanical Sympathy rule and is safe to
; call before the IDT can catch a divide error.
; =============================================================================

if ~ definite FP_SERIAL_INC
FP_SERIAL_INC = 1

; -----------------------------------------------------------------------------
; COM1 register port offsets and the init byte values
; -----------------------------------------------------------------------------
SERIAL_PORT         = 0x3F8     ; COM1 base
SERIAL_THR          = SERIAL_PORT + 0   ; transmit holding register (write)
SERIAL_RBR          = SERIAL_PORT + 0   ; receive buffer register (read)
SERIAL_IER          = SERIAL_PORT + 1   ; interrupt enable register
SERIAL_FCR          = SERIAL_PORT + 2   ; FIFO control register (write)
SERIAL_LCR          = SERIAL_PORT + 3   ; line control register
SERIAL_MCR          = SERIAL_PORT + 4   ; modem control register
SERIAL_LSR          = SERIAL_PORT + 5   ; line status register

SERIAL_LCR_DLAB     = 0x80      ; divisor latch access bit
SERIAL_LCR_8N1      = 0x03      ; 8 data bits, no parity, 1 stop bit
SERIAL_FCR_ENABLE   = 0xC7      ; FIFO on, clear both FIFOs, 14-byte trigger
SERIAL_LSR_THRE     = 0x20      ; transmit holding register empty
SERIAL_LSR_DR       = 0x01      ; data ready (receive)

; =============================================================================
; fp$serial_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi, rcx
;     ERRORS    : none (fire-and-forget; the host either has COM1 or not)
;   Initializes COM1 at 115200 baud, 8N1, FIFO enabled and cleared.
; =============================================================================
align 16
fp$serial_init:
	mov	rdx, SERIAL_IER
	xor	eax, eax
	out	dx, al			; IER = 0 : interrupts disabled
	mov	rdx, SERIAL_LCR
	mov	al, SERIAL_LCR_DLAB
	out	dx, al			; LCR = 0x80 : DLAB on (baud divisor access)
	mov	rdx, SERIAL_PORT
	mov	al, 0x01
	out	dx, al			; DLL = 1 : divisor low (115200 baud)
	mov	rdx, SERIAL_IER
	xor	eax, eax
	out	dx, al			; DLM = 0 : divisor high
	mov	rdx, SERIAL_LCR
	mov	al, SERIAL_LCR_8N1
	out	dx, al			; LCR = 0x03 : 8N1, DLAB off
	mov	rdx, SERIAL_FCR
	mov	al, SERIAL_FCR_ENABLE
	out	dx, al			; FCR = 0xC7 : FIFO on, clear, 14-byte trigger
	ret

; =============================================================================
; fp$serial_putchar
;   CONTRACT
;     IN        : rdi = character to write (low byte)
;     OUT       : nothing
;     CLOBBERS  : rax, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi, rcx
;     ERRORS    : none
;   Waits for the transmit holding register to empty, then writes the byte.
; =============================================================================
align 16
fp$serial_putchar:
	mov	rdx, SERIAL_LSR
      .wait:
	in	al, dx			; read line status
	test	al, SERIAL_LSR_THRE
	jz	.wait			; spin until THR empty
	mov	rax, rdi		; character into al
	mov	rdx, SERIAL_THR
	out	dx, al			; write the character
	ret

; =============================================================================
; fp$serial_write
;   CONTRACT
;     IN        : rdi = pointer to null-terminated string
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;     ERRORS    : none
;   Writes the string character by character until the NUL terminator.
; =============================================================================
align 16
fp$serial_write:
	mov	rcx, rdi		; cursor
      .loop:
	movzx	eax, byte [rcx]
	or	al, al
	jz	.done			; NUL terminator
	push	rcx
	mov	rdi, rax
	call	fp$serial_putchar
	pop	rcx
	inc	rcx
	jmp	.loop
      .done:
	ret

; =============================================================================
; fp$serial_write_hex
;   CONTRACT
;     IN        : rdi = 64-bit value
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;     ERRORS    : none
;   Writes "0x" followed by 16 lowercase hex digits, most significant first.
;   Pure shift/and nibble extraction — no division.
; =============================================================================
align 16
fp$serial_write_hex:
	push	rbx			; rbx = value (survives every call)
	push	r12			; r12 = nibble counter
	mov	rbx, rdi
	mov	rdi, fp$serial_hex_prefix
	call	fp$serial_write		; "0x"
	mov	r12d, 16		; 16 nibbles
      .digit:
	rol	rbx, 4			; bring next nibble into the low bits
	mov	rax, rbx
	and	rax, 0x0F
	cmp	al, 10
	jb	.dec
	add	al, 'a' - 10
	jmp	.emit
      .dec:
	add	al, '0'
      .emit:
	movzx	edi, al			; digit character
	call	fp$serial_putchar	; preserves rbx and r12
	dec	r12d
	jnz	.digit
	pop	r12
	pop	rbx
	ret

; =============================================================================
; fp$serial_write_dec
;   CONTRACT
;     IN        : rdi = 64-bit unsigned value
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;     ERRORS    : none
;   Writes the value as a decimal number with no leading zeros (except the
;   value 0 itself, which prints "0"). NO DIVISION: each digit is recovered
;   by repeated subtraction of its power of ten.
; =============================================================================
align 16
fp$serial_write_dec:
	mov	rcx, rdi		; value
	or	rcx, rcx
	jnz	.scan
	mov	edi, '0'		; special case: value 0 prints "0"
	call	fp$serial_putchar
	ret
      .scan:
	mov	rdx, fp$serial_pow10	; table of powers 10^18 .. 10^0, descending
      .find_power:
	mov	rax, [rdx]
	or	rax, rax
	jz	.done
	cmp	rcx, rax
	jae	.digit			; rcx >= this power: emit its digit
	add	rdx, 8
	jmp	.find_power
      .digit:
	xor	edi, edi		; digit accumulator
      .subtract:
	cmp	rcx, rax
	jb	.emit
	sub	rcx, rax		; repeated subtraction, no div
	inc	edi
	jmp	.subtract
      .emit:
	push	rcx
	push	rdx
	push	rax
	add	edi, '0'
	call	fp$serial_putchar
	pop	rax
	pop	rdx
	pop	rcx
	add	rdx, 8			; next (smaller) power of ten
	mov	rax, [rdx]
	or	rax, rax
	jz	.done
	jmp	.digit
      .done:
	ret

; -----------------------------------------------------------------------------
; Read-only data: the "0x" prefix and the descending power-of-ten table used
; by the decimal formatter.
; -----------------------------------------------------------------------------
fp$serial_hex_prefix db '0x', 0

align 8
fp$serial_pow10:
	dq 1000000000000000000	; 10^18
	dq 100000000000000000	; 10^17
	dq 10000000000000000	; 10^16
	dq 1000000000000000	; 10^15
	dq 100000000000000	; 10^14
	dq 10000000000000	; 10^13
	dq 1000000000000	; 10^12
	dq 100000000000		; 10^11
	dq 10000000000		; 10^10
	dq 1000000000		; 10^9
	dq 100000000		; 10^8
	dq 10000000		; 10^7
	dq 1000000		; 10^6
	dq 100000		; 10^5
	dq 10000		; 10^4
	dq 1000			; 10^3
	dq 100			; 10^2
	dq 10			; 10^1
	dq 1			; 10^0
	dq 0			; sentinel

end if ; FP_SERIAL_INC
