; =============================================================================
; FASM_PLUS OS Core — EXCEPTIONS.INC
; CPU exception handlers (vectors 0-31) installed in the IDT.
;
; Depends on: IDT.INC, SERIAL.INC
; Emits: handler entry stubs and the common handler in the code section.
;
; Philosophy
; ----------
; "Crash prevention without explanation" is the cardinal sin. When the CPU
; raises an exception the handler must (1) preserve every register so the
; post-mortem is truthful, (2) print a one-line report naming the vector,
; the faulting RIP, the error code, and CR2, and (3) halt the machine in a
; controlled hlt loop. Silent corruption is never acceptable; a stopped
; machine with a full report always is.
;
; Stack layout on entry to a handler (64-bit interrupt frame):
;   [rsp+0]   RIP
;   [rsp+8]   CS
;   [rsp+16]  RFLAGS
;   [rsp+24]  RSP   (only on privilege change)
;   [rsp+32]  SS    (only on privilege change)
; Some exceptions also push an error code BELOW RIP (pushed last, so it is
; at [rsp] on entry to the stub). Those that do NOT push an error code have
; the stub push a synthetic 0 so the common handler always sees a uniform
; frame: [rsp]=err, [rsp+8]=RIP, ...
;
; Error-code rule (which vectors push one):
;   push error code:  8, 10, 11, 12, 13, 14, 17, 21
;   no error code:    0,1,2,3,4,5,6,7,9,15,16,17?,18,19,20
;   (17 double-checks: #AC pushes an error code — it is in the "push" set)
; =============================================================================

if ~ definite FP_EXCEPTIONS_INC
FP_EXCEPTIONS_INC = 1

if ~ definite FP_IDT_INC
  err 'EXCEPTIONS.INC requires IDT.INC (and SERIAL.INC) first'
end if

; =============================================================================
; Handler stubs
; -------------
; Each vector has a tiny stub that (a) pushes a synthetic 0 error code if the
; CPU did not push one, (b) pushes the vector number, and (c) jumps to the
; common handler. Because the CPU may already have pushed an error code, the
; stub normalizes the frame so fp$exception_handler always sees:
;   [rsp]   = vector number
;   [rsp+8] = error code (real or synthetic 0)
;   [rsp+16]= RIP, [rsp+24]=CS, [rsp+32]=RFLAGS, ...
; =============================================================================

; macro for vectors where the CPU pushes NO error code
macro EXC_NOERR num
{
align 16
fp$exc_stub_#num:
	push	0			; synthetic error code
	push	num			; vector number
	jmp	fp$exception_handler
}

; macro for vectors where the CPU DOES push an error code
macro EXC_ERR num
{
align 16
fp$exc_stub_#num:
	push	num			; vector number (error code already on stack)
	jmp	fp$exception_handler
}

; generate all 32 stubs with the correct error-code behavior
EXC_NOERR 0	; #DE divide error
EXC_NOERR 1	; #DB debug
EXC_NOERR 2	; NMI
EXC_NOERR 3	; #BP breakpoint
EXC_NOERR 4	; #OF overflow
EXC_NOERR 5	; #BR bound range
EXC_NOERR 6	; #UD invalid opcode
EXC_NOERR 7	; #NM device not available
EXC_ERR   8	; #DF double fault (error code, always 0)
EXC_NOERR 9	; coprocessor segment overrun (legacy)
EXC_ERR   10	; #TS invalid TSS
EXC_ERR   11	; #NP segment not present
EXC_ERR   12	; #SS stack segment fault
EXC_ERR   13	; #GP general protection
EXC_ERR   14	; #PF page fault
EXC_NOERR 15	; reserved
EXC_NOERR 16	; #MF x87 floating point
EXC_ERR   17	; #AC alignment check
EXC_NOERR 18	; #MC machine check
EXC_NOERR 19	; #XM SIMD floating point
EXC_NOERR 20	; #VE virtualization
EXC_ERR   21	; #CP control protection
EXC_NOERR 22	; reserved
EXC_NOERR 23	; reserved
EXC_NOERR 24	; reserved
EXC_NOERR 25	; reserved
EXC_NOERR 26	; reserved
EXC_NOERR 27	; reserved
EXC_NOERR 28	; reserved
EXC_NOERR 29	; reserved
EXC_NOERR 30	; reserved
EXC_NOERR 31	; reserved

; =============================================================================
; fp$exception_handler — common exception handler
;   CONTRACT
;     IN        : normalized frame on stack (vector, err, RIP, CS, RFLAGS)
;     OUT       : never returns (halts)
;     CLOBBERS  : all
;     PRESERVES : nothing meaningful (machine halts)
;   Saves every general register, prints
;     EXC <vec> RIP=<rip> ERR=<err> CR2=<cr2>
;   then halts in an infinite loop. No division is used; all numbers go
;   through the serial hex/decimal formatters.
; =============================================================================
align 16
fp$exception_handler:
	; preserve all general registers before touching anything
	PUSHAQ
	; after PUSHAQ the stack is: [rsp]=rdi..[rsp+56]=rax, then the
	; vector at [rsp+64], error code at [rsp+72], RIP at [rsp+80]
	; print "EXC "
	mov	rdi, fp$exc_str
	call	fp$serial_write
	; vector number
	mov	rdi, [rsp + 64]
	call	fp$serial_write_dec
	; " RIP="
	mov	rdi, fp$exc_rip_str
	call	fp$serial_write
	mov	rdi, [rsp + 80]
	call	fp$serial_write_hex
	; " ERR="
	mov	rdi, fp$exc_err_str
	call	fp$serial_write
	mov	rdi, [rsp + 72]
	call	fp$serial_write_hex
	; " CR2="
	mov	rdi, fp$exc_cr2_str
	call	fp$serial_write
	mov	rdi, cr2
	call	fp$serial_write_hex
	; newline
	mov	rdi, fp$exc_nl_str
	call	fp$serial_write
	; restore registers for a truthful post-mortem, then halt
	POPAQ
      .halt:
	cli
	hlt
	jmp	.halt

; =============================================================================
; fp$exceptions_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, rsi, flags
;     PRESERVES : rbx, rbp, r12-r15
;     ERRORS    : none
;   Installs all 32 exception handler stubs into IDT vectors 0-31.
; =============================================================================
align 16
fp$exceptions_init:
	mov	rcx, fp$exc_stub_table	; table of 32 stub addresses
	xor	edi, edi		; vector 0
      .install:
	mov	rsi, [rcx]
	push	rcx
	push	rdi
	call	fp$idt_set_entry
	pop	rdi
	pop	rcx
	add	rcx, 8
	inc	edi
	cmp	edi, 32
	jb	.install
	ret

; -----------------------------------------------------------------------------
; Read-only data: report strings and the stub address table (in IDT order).
; -----------------------------------------------------------------------------
fp$exc_str	db 'EXC ', 0
fp$exc_rip_str	db ' RIP=', 0
fp$exc_err_str	db ' ERR=', 0
fp$exc_cr2_str	db ' CR2=', 0
fp$exc_nl_str	db 10, 0

align 8
fp$exc_stub_table:
	dq fp$exc_stub_0,  fp$exc_stub_1,  fp$exc_stub_2,  fp$exc_stub_3
	dq fp$exc_stub_4,  fp$exc_stub_5,  fp$exc_stub_6,  fp$exc_stub_7
	dq fp$exc_stub_8,  fp$exc_stub_9,  fp$exc_stub_10, fp$exc_stub_11
	dq fp$exc_stub_12, fp$exc_stub_13, fp$exc_stub_14, fp$exc_stub_15
	dq fp$exc_stub_16, fp$exc_stub_17, fp$exc_stub_18, fp$exc_stub_19
	dq fp$exc_stub_20, fp$exc_stub_21, fp$exc_stub_22, fp$exc_stub_23
	dq fp$exc_stub_24, fp$exc_stub_25, fp$exc_stub_26, fp$exc_stub_27
	dq fp$exc_stub_28, fp$exc_stub_29, fp$exc_stub_30, fp$exc_stub_31

end if ; FP_EXCEPTIONS_INC
