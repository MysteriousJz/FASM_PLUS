; =============================================================================
; FASM_PLUS OS Core — BOOT_STUB.ASM
; 32-bit Multiboot2 entry point and the switch to 64-bit long mode.
;
; Depends on: PAGING.INC, GDT.INC (selector constants), CONSTANTS.INC
; Emits: the Multiboot2 header, the 32-bit entry stub, and the far jump into
;        the 64-bit kernel. This file is assembled as `use32`; the trailing
;        64-bit half is emitted by the REX.W-patched FASM encoder and becomes
;        valid the instant the CPU enters long mode.
;
; Memory map (locked, from the OS core specification)
; ---------------------------------------------------
;   0x70000            kernel stack top (grows down)
;   0x90000-0x9FFFF    page tables: PML4 at 0x90000, PDPTs, PDs (9 pages)
;   0x100000           kernel ELF entry point (_start)
;   0x00000000-0xFFFFFFFF  identity-mapped first 4 GB via 2 MB large pages
;
; The CPU arrives in 32-bit protected mode (Multiboot2). We never touch
; hardware we do not need: no A20 (QEMU enables it), no PIC (KERNEL_INIT
; remaps it later), just enough to reach long mode.
;
; Multiboot2 header (must be within the first 8 KB of the image, 8-byte
; aligned, fields little-endian):
;   magic    = 0xE85250D6
;   arch     = 0 (i386)
;   length   = header_end - header_start
;   checksum = -(magic + arch + length) mod 2^32
;   then tagged entries; we terminate with the end tag (0,0,8).
; =============================================================================

if ~ definite FP_BOOT_STUB_ASM
FP_BOOT_STUB_ASM = 1

if ~ definite FP_PAGING_INC
  err 'BOOT_STUB.ASM requires PAGING.INC (and GDT.INC, CONSTANTS.INC) first'
end if

; -----------------------------------------------------------------------------
; Multiboot constants.
;
; The OS core targets Multiboot2 (the modern, extensible spec). However the
; QEMU system emulator available for CI (8.2) only loads 32-bit ELF images via
; the original Multiboot (v1) protocol for `-kernel`, and rejects both ELF64
; class images and PVH ELF notes. To keep `qemu-system-x86_64 -kernel` as the
; test harness we emit a Multiboot1 header by default and accept BOTH loader
; magics on entry. Set FP_MB2_HEADER = 1 before including this file to emit a
; Multiboot2 header instead (for a Multiboot2-capable loader such as GRUB2).
; -----------------------------------------------------------------------------
MB1_MAGIC       = 0x1BADB002    ; Multiboot1 header magic
MB1_ENTRY_MAGIC = 0x2BADB002    ; EAX on entry from a Multiboot1 loader
MB2_MAGIC       = 0xE85250D6
MB2_ARCH_I386   = 0
MB2_ENTRY_MAGIC = 0x36D76289	; EAX on entry from a Multiboot2 loader

if ~ definite FP_MB2_HEADER
FP_MB2_HEADER = 0		; default: Multiboot1 (QEMU -kernel compatible)
end if

; fixed physical addresses from the locked memory map
KERNEL_STACK_TOP = 0x70000
PML4_BASE        = 0x90000

; CR0/CR4/EFER bits for the long-mode switch
CR0_PG          = 0x80000000    ; paging enable
CR0_PE          = 0x00000001    ; protection enable
CR4_PAE         = 0x00000020    ; physical address extension
EFER_MSR        = 0xC0000080
EFER_LME        = 0x00000100    ; long mode enable

; =============================================================================
; Boot header (assembled first, into the .multiboot_header section)
; This is data, not code; it must be findable by the loader in the first 8 KB.
; FP_MULTIBOOT_HEADER emits a Multiboot1 header (default, QEMU -kernel) or a
; Multiboot2 header (FP_MB2_HEADER = 1, for GRUB2 and other v2 loaders).
; =============================================================================
macro FP_MULTIBOOT_HEADER
{
  if FP_MB2_HEADER
	; Multiboot2 header: magic, arch, length, checksum, end tag
align 8
fp$mb_header_start:
	dd MB2_MAGIC
	dd MB2_ARCH_I386
	dd fp$mb_header_end - fp$mb_header_start
	dd -(MB2_MAGIC + MB2_ARCH_I386 + (fp$mb_header_end - fp$mb_header_start))
	; end tag (type 0, flags 0, size 8)
align 8
	dw 0
	dw 0
	dd 8
fp$mb_header_end:
  else
	; Multiboot1 header: magic, flags(0), checksum. Must be 4-byte aligned.
	; The ELF program headers already describe the load, so no a.out kludge.
align 4
fp$mb_header_start:
	dd MB1_MAGIC
	dd 0
	dd -(MB1_MAGIC + 0)
fp$mb_header_end:
  end if
}

; =============================================================================
; FP_BOOT_STUB_32 — the 32-bit `_start` entry point, emitted as a macro so
; OS_TEST.ASM can place it in the low use32 segment.
;   CONTRACT
;     IN        : EAX = loader magic (Multiboot1 0x2BADB002 or Multiboot2
;                 0x36D76289), EBX = loader info pointer
;     OUT       : never returns to the loader; ends in fp$kernel_init
;     CLOBBERS  : everything
;   Verifies the loader magic, builds the identity page tables, enables PAE
;   and long mode, turns on paging, and far-jumps into the 64-bit kernel.
;   On any failure it prints a one-letter code to the serial port (which the
;   32-bit stub initializes inline) and halts.
; =============================================================================
macro FP_BOOT_STUB_32
{
align 16
_start:
	; interrupts off for the whole mode switch
	cli
	; verify the loader magic; on mismatch print 'M' and halt.
	; Accept both Multiboot1 (0x2BADB002) and Multiboot2 (0x36D76289) so the
	; same image boots under QEMU -kernel (v1) and a Multiboot2 loader.
	cmp	eax, MB1_ENTRY_MAGIC
	je	.magic_ok
	cmp	eax, MB2_ENTRY_MAGIC
	jne	.fail_magic
      .magic_ok:
	; initialize COM1 inline (before paging) so failures are visible.
	; This is the same 16550 sequence as fp$serial_init, written out
	; because the 64-bit serial functions are not reachable yet.
	mov	dx, 0x3F9		; IER
	xor	eax, eax
	out	dx, al
	mov	dx, 0x3FB		; LCR
	mov	al, 0x80
	out	dx, al			; DLAB on
	mov	dx, 0x3F8		; DLL
	mov	al, 0x01
	out	dx, al			; 115200 baud low byte
	mov	dx, 0x3F9		; DLM
	xor	eax, eax
	out	dx, al
	mov	dx, 0x3FB		; LCR
	mov	al, 0x03
	out	dx, al			; 8N1, DLAB off
	mov	dx, 0x3FA		; FCR
	mov	al, 0xC7
	out	dx, al			; FIFO on

	; ---------------------------------------------------------------------
	; Build the identity page tables at 0x90000 (PML4), 0x91000-0x94000
	; (PDPT0..3), 0x95000-0x98000 (PD0..3) — 9 pages, covering 4 GB.
	; This is a 32-bit inline implementation; the 64-bit
	; fp$paging_identity_map_4gb provides the same structure for the
	; kernel proper. Every entry is written with explicit 32-bit stores
	; because addresses below 4 GB fit in a dword.
	; ---------------------------------------------------------------------
	; clear the 9-page region [0x90000, 0x99000)
	mov	edi, PML4_BASE
	mov	ecx, 0x9000 shr 2	; dword count (no division)
      .clear:
	mov	dword [edi], 0
	add	edi, 4
	dec	ecx
	jnz	.clear
	; PML4[0..3] -> PDPT0..3 (present + writable)
	mov	edi, PML4_BASE
	mov	esi, PML4_BASE + 0x1000	; PDPT0
	mov	ecx, 4
      .pml4:
	mov	eax, esi
	or	eax, PTE_P_BIT or PTE_RW_BIT
	mov	[edi], eax
	mov	dword [edi + 4], 0	; high dword of the entry
	add	edi, 8
	add	esi, 0x1000
	dec	ecx
	jnz	.pml4
	; PDPTn[0] -> PDn (present + writable)
	mov	edi, PML4_BASE + 0x1000	; PDPT0
	mov	esi, PML4_BASE + 0x5000	; PD0
	mov	ecx, 4
      .pdpt:
	mov	eax, esi
	or	eax, PTE_P_BIT or PTE_RW_BIT
	mov	[edi], eax
	mov	dword [edi + 4], 0
	add	edi, 0x1000		; next PDPT page
	add	esi, 0x1000		; next PD page
	dec	ecx
	jnz	.pdpt
	; fill PD0..3 with 512 entries each of 2 MB large pages
	mov	edi, PML4_BASE + 0x5000	; PD0
	xor	esi, esi		; physical address cursor (starts at 0)
	mov	ecx, 4 * 512		; 4 PDs x 512 entries (shift, no mul)
      .pd:
	mov	eax, esi
	or	eax, PTE_P_BIT or PTE_RW_BIT or PTE_PS_BIT
	mov	[edi], eax
	mov	dword [edi + 4], 0	; physical addresses stay below 4 GB
	add	edi, 8
	add	esi, 0x200000		; next 2 MB
	dec	ecx
	jnz	.pd

	; ---------------------------------------------------------------------
	; Enable PAE, then long mode, then paging.
	; ---------------------------------------------------------------------
	mov	eax, cr4
	or	eax, CR4_PAE
	mov	cr4, eax		; CR4.PAE = 1
	mov	eax, PML4_BASE
	mov	cr3, eax		; load the PML4
	mov	ecx, EFER_MSR
	rdmsr
	or	eax, EFER_LME
	wrmsr				; EFER.LME = 1
	mov	eax, cr0
	or	eax, CR0_PG or CR0_PE
	mov	cr0, eax		; CR0.PG = 1 : paging on, long mode active

	; ---------------------------------------------------------------------
	; Load a minimal 64-bit GDT (built inline in .data) and far-jump into
	; the 64-bit kernel segment. The GDT here is a superset of the one
	; fp$gdt_init installs; both use the same selectors so the switch is
	; seamless.
	; ---------------------------------------------------------------------
	lgdt	[fp$boot_gdtr]
	jmp	GDT_KERNEL_CODE:fp$long_mode_entry
	; NOTE on the far jump: in a use32 section FASM emits
	;   EA <offset32> <selector16>
	; which is the exact 64-bit far-jump encoding. The REX.W-patched
	; encoder accepts this and emits it unchanged; only after the jump is
	; the CPU in long mode and the 64-bit code below valid.

	; ---------------------------------------------------------------------
	; Failure paths: print a one-letter code to COM1 and halt.
	; ---------------------------------------------------------------------
      .fail_magic:
	mov	dx, 0x3F8
	mov	al, 'M'
	out	dx, al
      .halt:
	cli
	hlt
	jmp	.halt
} ; end FP_BOOT_STUB_32

; =============================================================================
; FP_LONG_MODE_ENTRY — 64-bit entry point, emitted as a macro so OS_TEST.ASM
; can place it in the 64-bit code segment (the stub above is 32-bit).
;   CONTRACT
;     IN        : running in 64-bit long mode, identity-mapped, CS=kernel
;     OUT       : never returns; ends in fp$kernel_init then the test runner
;     CLOBBERS  : everything
;   Sets up the 64-bit data segments and the kernel stack, then calls the
;   kernel initializer and the test runner. Valid only after the far jump.
; =============================================================================
macro FP_LONG_MODE_ENTRY
{
align 16
fp$long_mode_entry:
	; 64-bit data segments
	mov	ax, GDT_KERNEL_DATA
	mov	ds, ax
	mov	es, ax
	mov	ss, ax
	; kernel stack at 0x70000, growing down
	mov	rsp, KERNEL_STACK_TOP
	and	rsp, -16		; 16-byte align the stack top
	; into the kernel proper
	call	fp$kernel_init
	; after init, run the test suite (defined in OS_TEST.ASM)
	call	fp$run_all_tests
	; fp$run_all_tests ends in fp$test_summary_and_exit, which halts; if it
	; ever returned we halt here as a backstop.
      .hang:
	cli
	hlt
	jmp	.hang
}

end if ; FP_BOOT_STUB_ASM
