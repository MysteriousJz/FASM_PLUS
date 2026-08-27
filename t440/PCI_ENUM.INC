; =============================================================================
; FASM_PLUS T440 Layer — PCI_ENUM.INC
; PCI bus enumeration: discover every device on the PCI bus.
;
; Depends on: CORDIS.INC (this driver is a Cordis fiber), SERIAL.INC
; Emits: functions in the current code section; the device table lives in BSS.
;
; Cordis integration
; ------------------
;   Fiber name : PCI_ENUMERATOR (fiber id T440_FIBER_PCI)
;   Provides   : PCI_DEVICE_TABLE  (coeffect key; value = base of the table)
;   Injects    : nothing (root provider)
;   Effects    : the device-table population is recorded so an unload returns
;                the table to empty (temporal composability). Reading PCI
;                config space has no hardware side effects, so there is no
;                hardware inverse to run — only the table is reverted.
;
; Why PCI first
; -------------
; Every other T440 driver (display, disk, network) needs to know which
; devices exist and where their registers are mapped. PCI config space is
; reachable with two I/O ports and no BIOS, so it works identically in QEMU
; and on the physical ThinkPad T440 (both present a standard PCI host bridge).
;
; PCI configuration mechanism #1 (PCI Local Bus Spec 2.2, Chapter 6)
; -------------------------------------------------------------------
;   Port 0xCF8  CONFIG_ADDRESS (32-bit, write):
;                 bit 31      enable
;                 bits 23:16  bus number
;                 bits 15:11  device number (0..31)
;                 bits 10:8   function number (0..7)
;                 bits 7:2    register offset / 4
;   Port 0xCFC  CONFIG_DATA (32-bit, read/write the selected dword)
;
; Configuration space dword offsets of interest:
;   0x00  Device ID (high 16) | Vendor ID (low 16)   — 0xFFFF vendor = absent
;   0x08  Class Code (bits 31:24) | Subclass (23:16) | Prog IF (15:8) | Rev
;   0x10..0x24  BAR0..BAR5 (base address registers)
;
; The device table is fixed-size (no dynamic allocation): at most
; PCI_MAX_DEVICES entries are kept, which is ample for a laptop (a T440
; exposes well under 64 functions).
; =============================================================================

if ~ definite FP_PCI_ENUM_INC
FP_PCI_ENUM_INC = 1

if ~ definite FP_CORDIS_INC
  err 'PCI_ENUM.INC requires CORDIS.INC (and SERIAL.INC) first'
end if

; -----------------------------------------------------------------------------
; PCI I/O ports and constants
; -----------------------------------------------------------------------------
PCI_CONFIG_ADDRESS = 0xCF8
PCI_CONFIG_DATA    = 0xCFC
PCI_ENABLE_BIT     = 0x80000000
PCI_VENDOR_ABSENT  = 0xFFFF

; config-space register offsets (byte offsets; the low 2 bits are dropped)
PCI_REG_ID         = 0x00        ; vendor + device id
PCI_REG_CLASS      = 0x08        ; class/subclass/progif/revision
PCI_REG_BAR0       = 0x10        ; first base address register

; table capacity
PCI_MAX_DEVICES    = 64

; -----------------------------------------------------------------------------
; PCI device table entry (fixed size, 64 bytes = one per device)
; -----------------------------------------------------------------------------
struc pci_dev {
    .present   dq ?               ; 0 = empty slot, 1 = occupied
    .bus       dq ?               ; bus number
    .device    dq ?               ; device number (0..31)
    .function  dq ?               ; function number (0..7)
    .vendor_id dq ?               ; 16-bit vendor id
    .device_id dq ?               ; 16-bit device id
    .classcode dq ?               ; class/subclass/progif (24 bits, low)
    .bar0      dq ?               ; BAR0 raw value (memory or I/O base)
}
virtual at 0
    pci_dev pci_dev
    pci_dev_end:
end virtual
PCI_DEV_SIZE = pci_dev_end

; Coeffect key this fiber provides (a token naming the provision). The value
; stored under the key is the physical/virtual base of the device table.
COEFFECT_PCI_DEVICE_TABLE = 0x7001

; =============================================================================
; fp$pci_config_address — build the 32-bit CONFIG_ADDRESS value
;   CONTRACT
;     IN        : rdi = bus, rsi = device, rdx = function, rcx = register offset
;     OUT       : rax = CONFIG_ADDRESS dword
;     CLOBBERS  : rax, flags
;     PRESERVES : rdi, rsi, rdx, rcx, rbx, rbp, r8-r15
;     ERRORS    : none (inputs are masked to their field widths)
;   Assembles the address with shifts and ORs only — no division.
; =============================================================================
align 16
fp$pci_config_address:
	and	rdi, 0xFF		; bus (8 bits)
	and	rsi, 0x1F		; device (5 bits)
	and	rdx, 0x07		; function (3 bits)
	and	rcx, 0xFC		; register, 4-byte aligned (drop low 2 bits)
	mov	rax, rdi
	shl	rax, 16			; bus -> bits 23:16
	mov	r8, rsi
	shl	r8, 11			; device -> bits 15:11
	or	rax, r8
	mov	r8, rdx
	shl	r8, 8			; function -> bits 10:8
	or	rax, r8
	or	rax, rcx		; register offset -> bits 7:2
	bts	rax, 31			; enable bit (OR cannot encode 0x80000000)
	ret

; =============================================================================
; fp$pci_read_dword — read one 32-bit dword from PCI config space
;   CONTRACT
;     IN        : rdi = bus, rsi = device, rdx = function, rcx = register offset
;     OUT       : rax = dword value
;     CLOBBERS  : rax, rdx (port), flags
;     PRESERVES : rbx, rbp, rsi, rdi, r8-r15 (rcx consumed by address build)
;     ERRORS    : none
; =============================================================================
align 16
fp$pci_read_dword:
	push	rdx			; save function (address build uses rdx)
	push	rcx			; save register offset
	call	fp$pci_config_address	; rax = address
	mov	rdx, PCI_CONFIG_ADDRESS
	out	dx, eax			; select the dword
	mov	rdx, PCI_CONFIG_DATA
	in	eax, dx			; read it
	pop	rcx
	pop	rdx
	ret

; =============================================================================
; fp$pci_enumerate — scan the whole bus and fill the device table
;   CONTRACT
;     IN        : nothing
;     OUT       : rax = number of devices found
;     CLOBBERS  : rax, rcx, rdx, rsi, rdi, r8-r11, flags
;     PRESERVES : rbx, rbp, r12-r15 (used internally, saved)
;     ERRORS    : none (an absent device is normal, not an error)
;   Scans bus 0, devices 0-31, functions 0-7. For each present function it
;   records vendor/device/class/BAR0 in the next free table slot. The scan
;   stops early in a slot's absence only at function 0; multi-function devices
;   are fully walked. All loops use counters, never division.
; =============================================================================
align 16
fp$pci_enumerate:
	push	rbx
	push	r12
	push	r13
	push	r14
	push	r15
	; clear the table and the count
	mov	rdi, fp$pci_device_table
	mov	rcx, PCI_MAX_DEVICES * PCI_DEV_SIZE
      .clear:
	mov	byte [rdi], 0
	inc	rdi
	dec	rcx
	jnz	.clear
	mov	qword [fp$pci_device_count], 0
	xor	r15, r15		; r15 = table slot index (0..63)
	xor	r12, r12		; r12 = device number (bus 0 assumed)
      .dev_loop:
	cmp	r12, 32
	jae	.done
	xor	r13, r13		; r13 = function number
      .fn_loop:
	cmp	r13, 8
	jae	.next_dev
	; read vendor/device id at register 0
	mov	rdi, 0			; bus 0
	mov	rsi, r12		; device
	mov	rdx, r13		; function
	xor	rcx, rcx		; register 0
	call	fp$pci_read_dword
	mov	r14, rax		; r14 = device<<16 | vendor
	movzx	eax, ax			; vendor id (low 16)
	cmp	eax, PCI_VENDOR_ABSENT
	je	.maybe_skip_fn		; 0xFFFF = no device here
	; ----- device present: fill a table slot -----
	cmp	r15, PCI_MAX_DEVICES
	jae	.table_full
	; slot pointer = table + slot*size
	mov	rax, r15
	mov	rcx, PCI_DEV_SIZE
	imul	rax, rcx
	add	rax, fp$pci_device_table
	; present, bus, device, function
	mov	qword [rax + pci_dev.present], 1
	mov	qword [rax + pci_dev.bus], 0
	mov	[rax + pci_dev.device], r12
	mov	[rax + pci_dev.function], r13
	; vendor/device id
	mov	rdx, r14
	movzx	rcx, dx			; vendor
	mov	[rax + pci_dev.vendor_id], rcx
	shr	rdx, 16			; device id
	mov	[rax + pci_dev.device_id], rdx
	; class code (register 0x08, upper 24 bits)
	push	rax
	mov	rdi, 0
	mov	rsi, r12
	mov	rdx, r13
	mov	rcx, PCI_REG_CLASS
	call	fp$pci_read_dword
	shr	rax, 8			; drop revision byte -> class/sub/progif
	mov	rcx, rax
	pop	rax
	mov	[rax + pci_dev.classcode], rcx
	; BAR0 (register 0x10)
	push	rax
	mov	rdi, 0
	mov	rsi, r12
	mov	rdx, r13
	mov	rcx, PCI_REG_BAR0
	call	fp$pci_read_dword
	mov	rcx, rax
	pop	rax
	mov	[rax + pci_dev.bar0], rcx
	; advance slot and count
	inc	r15
	inc	qword [fp$pci_device_count]
      .table_full:
      .maybe_skip_fn:
	; if function 0 is absent, no other function exists on this device
	or	r13, r13
	jnz	.next_fn
	; function 0 absent -> skip remaining functions of this device
	jmp	.next_dev
      .next_fn:
	inc	r13
	jmp	.fn_loop
      .next_dev:
	inc	r12
	jmp	.dev_loop
      .done:
	mov	rax, [fp$pci_device_count]
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret

; =============================================================================
; fp$pci_find_class — find the first device matching a class/subclass
;   CONTRACT
;     IN        : rdi = class code (upper 8 bits), rsi = subclass (8 bits)
;     OUT       : rax = table slot index, or -1 if not found
;     CLOBBERS  : rax, rcx, rdx, flags
;     PRESERVES : rbx, rbp, rsi, rdi, r8-r15
;     ERRORS    : -1 in rax when no device matches
;   The stored classcode is (class<<16)|(subclass<<8)|progif. We compare the
;   upper 16 bits (class+subclass) so a caller can match a device family.
; =============================================================================
align 16
fp$pci_find_class:
	and	rdi, 0xFF		; class
	and	rsi, 0xFF		; subclass
	shl	rdi, 8
	or	rdi, rsi		; rdi = (class<<8)|subclass
	shl	rdi, 8			; -> (class<<16)|(subclass<<8); low byte 0
	mov	rcx, fp$pci_device_table
	xor	edx, edx
      .scan:
	cmp	rdx, [fp$pci_device_count]
	jae	.notfound
	mov	rax, [rcx + pci_dev.present]
	test	rax, rax
	jz	.next
	; classcode = (class<<16)|(subclass<<8)|progif; compare class+subclass
	mov	rax, [rcx + pci_dev.classcode]
	and	eax, 0x00FFFF00		; keep class (23:16) + subclass (15:8)
	cmp	rax, rdi
	je	.found
      .next:
	add	rcx, PCI_DEV_SIZE
	inc	edx
	jmp	.scan
      .found:
	mov	rax, rdx
	ret
      .notfound:
	mov	rax, -1
	ret

; =============================================================================
; fp$pci_enum_apply — the Cordis fiber's apply function
;   CONTRACT
;     IN        : nothing (the fiber id is the constant T440_FIBER_PCI)
;     OUT       : rax = 0 on success
;     CLOBBERS  : all
;     ERRORS    : E_OUT_OF_MEMORY if the provision cannot be recorded
;   Runs the enumeration, then provides PCI_DEVICE_TABLE with the table base
;   as its value. The table population is registered as a revertible effect
;   whose inverse clears the table (temporal composability).
; =============================================================================
align 16
fp$pci_enum_apply:
	; record the revertible effect FIRST so an unload restores an empty
	; table even if a later step fails (LIFO ordering keeps this safe).
	mov	rdi, T440_FIBER_PCI
	mov	rsi, fp$pci_enum_inverse
	xor	rdx, rdx
	call	fp$effect_record
	test	rax, rax
	js	.fail
	; run the enumeration
	call	fp$pci_enumerate
	; provide the device table to dependents
	mov	rdi, T440_FIBER_PCI
	mov	rsi, COEFFECT_PCI_DEVICE_TABLE
	mov	rdx, fp$pci_device_table
	call	fp$coeffect_provide
	test	rax, rax
	js	.fail
	xor	eax, eax
	ret
      .fail:
	mov	rax, -1
	ret

; fp$pci_enum_inverse — revert the enumeration (clear the table + count).
;   CONTRACT: IN rdi = context (ignored); CLOBBERS rax, rcx, rdi, flags.
align 16
fp$pci_enum_inverse:
	mov	rdi, fp$pci_device_table
	mov	rcx, PCI_MAX_DEVICES * PCI_DEV_SIZE
      .clear:
	mov	byte [rdi], 0
	inc	rdi
	dec	rcx
	jnz	.clear
	mov	qword [fp$pci_device_count], 0
	ret

end if ; FP_PCI_ENUM_INC
