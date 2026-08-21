; =============================================================================
; FASM_PLUS OS Core — GDT.INC
; Global Descriptor Table and Task State Segment.
;
; Depends on: CONSTANTS.INC, SERIAL.INC
; Emits: functions in the current code section; the GDT image and GDTR live
;        in the current data area (allocated by OS_TEST.ASM).
;
; In long mode segmentation is vestigial — the GDT exists to carry privilege
; levels (ring 0 kernel, ring 3 user), the 64-bit code/data descriptors, and
; the TSS descriptor used by the CPU on ring transitions (IST/RSP0).
;
; Segment selector layout
; -----------------------
;   bits 15:3 = GDT index, bit 2 = TI (0=GDT), bits 1:0 = RPL
;
; Required entries (index : selector):
;   0 : 0x00  null
;   1 : 0x08  kernel code, ring 0, 64-bit
;   2 : 0x10  kernel data, ring 0
;   3 : 0x1B  user   code, ring 3, 64-bit   (0x18 | 3)
;   4 : 0x23  user   data, ring 3           (0x20 | 3)
;   5 : 0x28  TSS, ring 0 (16-byte descriptor, occupies slots 5 and 6)
;
; 64-bit code/data descriptor format (8 bytes):
;   bytes 0-1  limit 15:0      (ignored in 64-bit; set 0xFFFF)
;   bytes 2-3  base 15:0       (ignored; set 0)
;   byte  4    base 23:16      (ignored; set 0)
;   byte  5    access          (P DPL S E DC RW A)
;   byte  6    granularity     (G DB L reserved limit 19:16)
;   byte  7    base 31:24      (ignored; set 0)
;
; TSS descriptor format (16 bytes):
;   Same low 8 bytes as a system descriptor (type 1001b = available 64-bit
;   TSS), plus bytes 8-15 = base 63:32.
; =============================================================================

if ~ definite FP_GDT_INC
FP_GDT_INC = 1

if ~ definite FP_SERIAL_INC
  err 'GDT.INC requires SERIAL.INC (and CONSTANTS.INC) first'
end if

; -----------------------------------------------------------------------------
; Selectors (index shl 3, TI=0; user entries get RPL=3)
; -----------------------------------------------------------------------------
GDT_NULL        = 0x00
GDT_KERNEL_CODE = 0x08
GDT_KERNEL_DATA = 0x10
GDT_USER_CODE   = 0x1B
GDT_USER_DATA   = 0x23
GDT_TSS         = 0x28

; Access byte bits
GDT_ACC_P       = 0x80          ; present
GDT_ACC_DPL3    = 0x60          ; ring 3
GDT_ACC_S       = 0x10          ; code/data (not system)
GDT_ACC_E       = 0x08          ; executable
GDT_ACC_RW      = 0x02          ; readable (code) / writable (data)
GDT_ACC_TSS64   = 0x09          ; available 64-bit TSS (system, P set below)

; Granularity byte bits
GDT_GRAN_G      = 0x80          ; 4KB granularity
GDT_GRAN_L      = 0x20          ; long mode (64-bit code)
GDT_GRAN_LIM    = 0x0F          ; limit 19:16 field (set 0xF)

; =============================================================================
; fp$gdt_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none
;   Loads the GDT with lgdt and reloads the data segment registers with the
;   kernel data selector. The code segment is reloaded with a far return
;   through the kernel code selector so CS matches immediately.
; =============================================================================
align 16
fp$gdt_init:
	; build the GDT image into the reserved data area
	mov	rcx, fp$gdt_image
	; entry 0: null
	xor	eax, eax
	mov	[rcx + GDT_NULL], rax
	; entry 1: kernel code — present, ring0, S=1, E=1, RW=1; L=1, G=1
	mov	rax, GDT_ACC_P or GDT_ACC_S or GDT_ACC_E or GDT_ACC_RW
	shl	rax, 40			; access byte lands at byte 5
	mov	rdx, GDT_GRAN_G or GDT_GRAN_L or GDT_GRAN_LIM
	shl	rdx, 48			; granularity byte lands at byte 6
	or	rax, rdx
	mov	edx, 0xFFFF		; limit 15:0
	or	rax, rdx
	mov	[rcx + GDT_KERNEL_CODE], rax
	; entry 2: kernel data — present, ring0, S=1, RW=1; L=0, G=1
	mov	rax, GDT_ACC_P or GDT_ACC_S or GDT_ACC_RW
	shl	rax, 40
	mov	rdx, GDT_GRAN_G or GDT_GRAN_LIM
	shl	rdx, 48
	or	rax, rdx
	mov	edx, 0xFFFF
	or	rax, rdx
	mov	[rcx + GDT_KERNEL_DATA], rax
	; entry 3: user code — present, ring3, S=1, E=1, RW=1; L=1, G=1
	mov	rax, GDT_ACC_P or GDT_ACC_DPL3 or GDT_ACC_S or GDT_ACC_E or GDT_ACC_RW
	shl	rax, 40
	mov	rdx, GDT_GRAN_G or GDT_GRAN_L or GDT_GRAN_LIM
	shl	rdx, 48
	or	rax, rdx
	mov	edx, 0xFFFF
	or	rax, rdx
	mov	[rcx + GDT_USER_CODE], rax
	; entry 4: user data — present, ring3, S=1, RW=1; L=0, G=1
	mov	rax, GDT_ACC_P or GDT_ACC_DPL3 or GDT_ACC_S or GDT_ACC_RW
	shl	rax, 40
	mov	rdx, GDT_GRAN_G or GDT_GRAN_LIM
	shl	rdx, 48
	or	rax, rdx
	mov	edx, 0xFFFF
	or	rax, rdx
	mov	[rcx + GDT_USER_DATA], rax
	; entries 5-6: TSS descriptor — zeroed here; fp$gdt_load_tss fills base.
	xor	eax, eax
	mov	[rcx + GDT_TSS], rax
	mov	[rcx + GDT_TSS + 8], rax

	; load the GDT pointer and reload segments
	lgdt	[fp$gdtr]
	mov	rax, GDT_KERNEL_DATA
	mov	ds, rax
	mov	es, rax
	mov	ss, rax
	; reload CS via a far return through the kernel code selector
	push	GDT_KERNEL_CODE
	lea	rax, [.cs_reloaded]
	push	rax
	retfq
      .cs_reloaded:
	ret

; =============================================================================
; fp$gdt_load_tss
;   CONTRACT
;     IN        : rdi = TSS base address
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi
;     ERRORS    : none
;   Writes the TSS base into the 16-byte TSS descriptor (slots 5-6), sets the
;   descriptor present/available, and loads the TSS selector with ltr.
;   The descriptor is assembled field-by-field to keep every shift explicit.
; =============================================================================
align 16
fp$gdt_load_tss:
	mov	rcx, fp$gdt_image
	mov	rax, rdi		; rax = TSS base
	; byte 0-1: limit 15:0 = 0x67 (104-byte TSS minus 1)
	mov	rdx, 0x67
	; bytes 2-3: base 15:0
	mov	rsi, rax
	and	rsi, 0xFFFF
	shl	rsi, 16
	or	rdx, rsi
	; byte 4: base 23:16
	mov	rsi, rax
	and	rsi, 0xFF0000		; isolate base 23:16
	shl	rsi, 16			; into byte 4 position (bits 32-39)
	or	rdx, rsi
	; byte 5: access = present | available 64-bit TSS (0x89)
	mov	rsi, GDT_ACC_P or GDT_ACC_TSS64
	shl	rsi, 40
	or	rdx, rsi
	; byte 6: granularity 0 (limit 19:16 = 0, no flags)
	; byte 7: base 31:24
	mov	rsi, rax
	and	rsi, 0xFF000000		; isolate base 31:24
	shl	rsi, 32			; into byte 7 position (bits 56-63)
	or	rdx, rsi
	mov	[rcx + GDT_TSS], rdx
	; bytes 8-15: base 63:32 (upper qword of the 16-byte descriptor)
	mov	rdx, rax
	shr	rdx, 32
	mov	[rcx + GDT_TSS + 8], rdx
	; load the TSS selector
	mov	ax, GDT_TSS
	ltr	ax
	ret

; -----------------------------------------------------------------------------
; Read-only data: the GDTR (limit, base). The GDT image itself is writable and
; lives in the .data area allocated by OS_TEST.ASM as fp$gdt_image.
; -----------------------------------------------------------------------------
align 16
fp$gdtr:
	dw FP_GDT_IMAGE_SIZE - 1	; limit
	dq fp$gdt_image			; base

end if ; FP_GDT_INC
