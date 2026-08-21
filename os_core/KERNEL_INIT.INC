; =============================================================================
; FASM_PLUS OS Core — KERNEL_INIT.INC
; Bootstrap sequence run once after the long-mode switch.
;
; Depends on: all lower components (SERIAL, GDT, IDT, PAGING, EXCEPTIONS,
;             CONTEXT, SYSCALL)
; Emits: functions in the current code section.
;
; This is the first kernel code that runs in 64-bit long mode. It brings up
; every CPU-general subsystem in dependency order, ending with the boot
; banner. Each step is a single obvious call; if any step faults, the
; exception handler (already installed) reports it and halts.
;
; Sequence
; --------
;   1. serial      debugging output must exist first
;   2. GDT         privilege levels and the kernel segments
;   3. IDT         interrupt routing
;   4. exceptions  controlled-halt handlers for vectors 0-31
;   5. PIC         remap 8259 IRQs to 32-47, then mask all (we are polling)
;   6. PIT         8254 channel 0 at 100 Hz for the system tick
;   7. syscalls    SYSCALL/SYSRET MSRs for ring 3 service calls
;   8. banner      "OS_CORE BOOT OK"
; =============================================================================

if ~ definite FP_KERNEL_INIT_INC
FP_KERNEL_INIT_INC = 1

if ~ definite FP_SYSCALL_INC
  err 'KERNEL_INIT.INC requires SYSCALL.INC (and all lower components) first'
end if

; 8259 PIC ports
PIC1_COMMAND    = 0x20
PIC1_DATA       = 0x21
PIC2_COMMAND    = 0xA0
PIC2_DATA       = 0xA1
PIC_ICW1_INIT   = 0x11          ; initialize, ICW4 needed
PIC_ICW4_8086   = 0x01          ; 8086/88 mode
PIC_IRQ_BASE    = 32            ; IRQs remap to vectors 32-47

; 8254 PIT ports and the 100 Hz divisor
PIT_CHANNEL0    = 0x40
PIT_COMMAND     = 0x43
PIT_MODE_RATE   = 0x36          ; channel 0, lobyte/hibyte, rate generator
PIT_BASE_HZ     = 1193182       ; PIT input clock
PIT_TARGET_HZ   = 100
PIT_DIVISOR     = PIT_BASE_HZ / PIT_TARGET_HZ   ; 11931 (compile-time constant)

; =============================================================================
; fp$pic_remap
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi, rcx
;     ERRORS    : none
;   Remaps the 8259 PIC so IRQs 0-15 land on vectors 32-47 (out of the
;   exception range), then masks every IRQ line. The bootstrap polls devices
;   and takes no hardware interrupts, so the PIC is silenced after the remap.
; =============================================================================
align 16
fp$pic_remap:
	; ICW1: begin init (both PICs)
	mov	rdx, PIC1_COMMAND
	mov	al, PIC_ICW1_INIT
	out	dx, al
	mov	rdx, PIC2_COMMAND
	out	dx, al
	; ICW2: vector offsets (master 32, slave 40)
	mov	rdx, PIC1_DATA
	mov	al, PIC_IRQ_BASE
	out	dx, al
	mov	rdx, PIC2_DATA
	mov	al, PIC_IRQ_BASE + 8
	out	dx, al
	; ICW3: cascade wiring (master has slave on IRQ2; slave id 2)
	mov	rdx, PIC1_DATA
	mov	al, 0x04
	out	dx, al
	mov	rdx, PIC2_DATA
	mov	al, 0x02
	out	dx, al
	; ICW4: 8086 mode
	mov	rdx, PIC1_DATA
	mov	al, PIC_ICW4_8086
	out	dx, al
	mov	rdx, PIC2_DATA
	out	dx, al
	; OCW1: mask all IRQ lines (polling mode)
	mov	rdx, PIC1_DATA
	mov	al, 0xFF
	out	dx, al
	mov	rdx, PIC2_DATA
	out	dx, al
	ret

; =============================================================================
; fp$pit_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi, rcx
;     ERRORS    : none
;   Programs 8254 channel 0 as a rate generator at 100 Hz. The divisor is a
;   compile-time constant (base clock / target), so there is no runtime
;   division. The IRQ it would raise is masked at the PIC; the tick counter
;   is readable but the bootstrap does not consume it.
; =============================================================================
align 16
fp$pit_init:
	mov	rdx, PIT_COMMAND
	mov	al, PIT_MODE_RATE
	out	dx, al
	mov	rdx, PIT_CHANNEL0
	mov	al, PIT_DIVISOR and 0xFF	; lobyte
	out	dx, al
	mov	al, (PIT_DIVISOR shr 8) and 0xFF	; hibyte
	out	dx, al
	ret

; =============================================================================
; fp$kernel_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : all registers (this is the first kernel code)
;     PRESERVES : nothing
;     ERRORS    : none returned; a fault in any step is caught by the
;                 exception handlers and reported before the halt
;   Runs the full bootstrap sequence and prints the boot banner.
; =============================================================================
align 16
fp$kernel_init:
	call	fp$serial_init		; 1. serial
	call	fp$gdt_init		; 2. GDT
	call	fp$idt_init		; 3. IDT
	call	fp$exceptions_init	; 4. exception handlers
	call	fp$pic_remap		; 5. PIC -> vectors 32-47, masked
	call	fp$pit_init		; 6. PIT at 100 Hz
	call	fp$syscall_init		; 7. SYSCALL/SYSRET MSRs
	; 8. banner
	mov	rdi, fp$kernel_boot_ok
	call	fp$serial_write
	ret

; -----------------------------------------------------------------------------
; Read-only data: the boot banner.
; -----------------------------------------------------------------------------
fp$kernel_boot_ok db 'OS_CORE BOOT OK', 10, 0

end if ; FP_KERNEL_INIT_INC
