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

if ~ definite FP_HAS_DISPLAY
FP_HAS_DISPLAY = 0		; default: no display (bare OS core). The T440
				; image sets this to 1 and provides the display
				; mode block + the framebuffer diagnostics data.
end if

; Preferred graphics mode for the Multiboot2 framebuffer request tag. The T440
; panel is 1600x900, but 1024x768x32 is the safest VESA mode; GRUB2 picks the
; closest mode the firmware supports. These are only preferences.
MB2_REQ_WIDTH  = 1024
MB2_REQ_HEIGHT = 768
MB2_REQ_DEPTH  = 32

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
	; Multiboot2 header: magic, arch, length, checksum, then tags.
	; The framebuffer request tag (type 5) asks the loader to set a graphics
	; mode and pass the LFB back in the boot-information structure.
align 8
fp$mb_header_start:
	dd MB2_MAGIC
	dd MB2_ARCH_I386
	dd fp$mb_header_end - fp$mb_header_start
	dd -(MB2_MAGIC + MB2_ARCH_I386 + (fp$mb_header_end - fp$mb_header_start))
	; framebuffer request tag (type 5, flags 0, size 20): ask for a linear
	; graphics mode. width/height/depth are a *preference*; the loader picks
	; the closest mode it supports (0 = no preference).
align 8
	dw 5			; type = framebuffer request
	dw 0			; flags
	dd 20			; size
	dd MB2_REQ_WIDTH	; preferred width
	dd MB2_REQ_HEIGHT	; preferred height
	dd MB2_REQ_DEPTH	; preferred depth (bpp)
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
; =============================================================================
; FP_MB2_COPY_FB — parse the Multiboot2 boot-information structure and copy the
; framebuffer tag into the kernel's display mode block.
;   CONTRACT
;     IN        : EBX = physical address of the Multiboot2 info structure
;                 (valid only when the loader magic was 0x36D76289)
;     OUT       : fp$display_mode_block filled if a framebuffer tag is present
;     CLOBBERS  : eax, ecx, edx, esi, edi, flags
;   Runs in 32-bit protected mode before the long-mode switch, so the
;   framebuffer address is captured early and is available to 64-bit code.
;   The info structure is a sequence of 8-byte-aligned tags:
;     [0]  total size (u32)   [4] reserved (u32)
;     then tags: each = { u32 type, u32 size, payload... } padded to 8 bytes.
;   The framebuffer tag (type 8) payload:
;     +8  framebuffer_addr (u64)   +16 pitch (u32)   +20 width (u32)
;     +24 height (u32)             +28 bpp (u8)      +29 type (u8)
;   We copy addr/pitch/width/height/bpp into fp$display_mode_block. The
;   framebuffer address is a full 64-bit value (the T440's LFB can be above
;   4 GB), so both dwords of the address are preserved.
;   No division: tag advance uses (size + 7) and (not 7) for 8-byte alignment.
; =============================================================================
; =============================================================================
; 32-bit serial diagnostics (run before the long-mode switch, so the 64-bit
; serial functions are not yet reachable). These use COM1 at 0x3F8, which the
; stub initializes inline in FP_BOOT_STUB_32.
; =============================================================================

; FP_MB2_PUTCHAR — write one ASCII character (in AL) to COM1.
;   CLOBBERS: dx. Preserves everything else.
macro FP_MB2_PUTCHAR
{
	mov	dx, 0x3F8
	out	dx, al
}

; FP_MB2_PRINT_HEX32 — print EAX as 8 hex digits to COM1.
;   CLOBBERS: eax, ecx, edx, esi, flags.
macro FP_MB2_PRINT_HEX32
{
	local .digit, .n, .e
	mov	esi, eax		; save value
	mov	ecx, 8			; 8 nibbles
      .digit:
	rol	esi, 4
	mov	eax, esi
	and	eax, 0xF
	cmp	eax, 10
	jb	.n
	add	eax, 'a' - 10
	jmp	.e
      .n:
	add	eax, '0'
      .e:
	FP_MB2_PUTCHAR
	dec	ecx
	jnz	.digit
}

; FP_MB2_PRINT_DEC — print EAX as unsigned decimal to COM1.
;   Repeated-subtraction over a power-of-ten table: NO division. Prints at
;   least one digit (0 prints as "0").
;   CLOBBERS: eax, ecx, edx, esi, edi, flags.
macro FP_MB2_PRINT_DEC
{
	local .find, .floop, .emit, .subloop, .skip, .done
	mov	esi, eax		; value
	test	esi, esi
	jnz	.find
	; value is 0: print a single '0'
	mov	eax, '0'
	FP_MB2_PUTCHAR
	jmp	.done
      .find:
	mov	edi, fp$mb2_pow10
      .floop:
	mov	eax, [edi]
	test	eax, eax
	jz	.done			; table exhausted
	cmp	esi, eax
	jae	.emit
	add	edi, 4
	jmp	.floop
      .emit:
	; count how many times this power fits (repeated subtraction)
	xor	ecx, ecx		; digit
      .subloop:
	cmp	esi, eax
	jb	.skip
	sub	esi, eax
	inc	ecx
	jmp	.subloop
      .skip:
	mov	eax, ecx
	add	eax, '0'
	FP_MB2_PUTCHAR
	add	edi, 4			; next (smaller) power
	mov	eax, [edi]
	test	eax, eax
	jz	.done
	jmp	.emit
      .done:
}

; FP_MB2_PRINT_CSTR — print the NUL-terminated string at ESI to COM1.
;   CLOBBERS: eax, edx, esi, flags.
macro FP_MB2_PRINT_CSTR
{
	local .loop, .done
      .loop:
	movzx	eax, byte [esi]
	test	al, al
	jz	.done
	FP_MB2_PUTCHAR
	inc	esi
	jmp	.loop
      .done:
}

; FP_MB2_FB_DIAG — report the framebuffer handoff to serial.
;   If a framebuffer tag was copied (fp$mb2_fb_present = 1), print:
;     FB: LFB=0x........ W=.. H=.. P=.. BPP=..
;   Otherwise print "FB: no framebuffer tag".
;   Runs in 32-bit mode; uses the inline COM1 port. Clobbers eax, ecx, edx,
;   esi, edi, flags.
macro FP_MB2_FB_DIAG
{
	local .notag, .done
	cmp	dword [fp$mb2_fb_present], 1
	jne	.notag
	; "FB: LFB=0x"
	mov	esi, fp$mb2_str_lfb
	FP_MB2_PRINT_CSTR
	mov	eax, dword [fp$display_mode_block + display_mode.lfb + 4]
	FP_MB2_PRINT_HEX32		; high dword
	mov	eax, dword [fp$display_mode_block + display_mode.lfb]
	FP_MB2_PRINT_HEX32		; low dword
	; " W="
	mov	esi, fp$mb2_str_w
	FP_MB2_PRINT_CSTR
	mov	eax, dword [fp$display_mode_block + display_mode.width]
	FP_MB2_PRINT_DEC
	; " H="
	mov	esi, fp$mb2_str_h
	FP_MB2_PRINT_CSTR
	mov	eax, dword [fp$display_mode_block + display_mode.height]
	FP_MB2_PRINT_DEC
	; " P="
	mov	esi, fp$mb2_str_p
	FP_MB2_PRINT_CSTR
	mov	eax, dword [fp$display_mode_block + display_mode.pitch]
	FP_MB2_PRINT_DEC
	; " BPP="
	mov	esi, fp$mb2_str_bpp
	FP_MB2_PRINT_CSTR
	mov	eax, dword [fp$display_mode_block + display_mode.bpp]
	FP_MB2_PRINT_DEC
	; newline
	mov	al, 13
	FP_MB2_PUTCHAR
	mov	al, 10
	FP_MB2_PUTCHAR
	jmp	.done
      .notag:
	mov	esi, fp$mb2_str_nofb
	FP_MB2_PRINT_CSTR
	mov	al, 13
	FP_MB2_PUTCHAR
	mov	al, 10
	FP_MB2_PUTCHAR
      .done:
}

macro FP_MB2_COPY_FB
{
	local .scan, .next, .done, .found
	mov	esi, ebx		; esi = info structure base
	mov	ecx, [esi]		; ecx = total size
	test	ecx, ecx
	jz	.done
	add	ecx, esi		; ecx = end of structure
	add	esi, 8			; first tag
      .scan:
	cmp	esi, ecx
	jae	.done			; ran off the end
	mov	eax, [esi]		; tag type
	test	eax, eax
	jz	.done			; end tag (type 0)
	cmp	eax, 8			; framebuffer tag?
	je	.found
      .next:
	mov	edx, [esi + 4]		; tag size
	add	edx, 7
	and	edx, -8			; round up to 8 bytes
	add	esi, edx
	jmp	.scan
      .found:
	; copy the framebuffer fields into the display mode block. The block is
	; a sequence of qwords. The framebuffer address is a FULL 64-bit value
	; (the T440's LFB can be above 4 GB), so copy BOTH dwords of the address.
	; The other fields are 32-bit (or smaller) in the tag; zero their high
	; dwords.
	mov	eax, [esi + 8]		; framebuffer_addr low dword
	mov	dword [fp$display_mode_block + display_mode.lfb], eax
	mov	eax, [esi + 12]		; framebuffer_addr HIGH dword (preserve it)
	mov	dword [fp$display_mode_block + display_mode.lfb + 4], eax
	mov	eax, [esi + 20]		; width
	mov	dword [fp$display_mode_block + display_mode.width], eax
	mov	dword [fp$display_mode_block + display_mode.width + 4], 0
	mov	eax, [esi + 24]		; height
	mov	dword [fp$display_mode_block + display_mode.height], eax
	mov	dword [fp$display_mode_block + display_mode.height + 4], 0
	mov	eax, [esi + 16]		; pitch (bytes per scanline)
	mov	dword [fp$display_mode_block + display_mode.pitch], eax
	mov	dword [fp$display_mode_block + display_mode.pitch + 4], 0
	movzx	eax, byte [esi + 28]	; bpp
	mov	dword [fp$display_mode_block + display_mode.bpp], eax
	mov	dword [fp$display_mode_block + display_mode.bpp + 4], 0
	; record that a framebuffer tag was found (for the serial diagnostic)
	mov	dword [fp$mb2_fb_present], 1
      .done:
}

; =============================================================================
; FP_MB2_DATA — the data the 32-bit stub diagnostics use. Emitted into the
; current data area by the top-level image (OS_TEST_T440.ASM). All of it is
; read/written by 32-bit code before the long-mode switch, so it must be in a
; segment the stub can address (the .data region).
; =============================================================================
macro FP_MB2_DATA
{
align 4
fp$mb2_fb_present	dd 0			; 1 = a framebuffer tag was copied
fp$mb2_str_lfb		db 'FB: LFB=0x', 0
fp$mb2_str_w		db ' W=', 0
fp$mb2_str_h		db ' H=', 0
fp$mb2_str_p		db ' P=', 0
fp$mb2_str_bpp		db ' BPP=', 0
fp$mb2_str_nofb		db 'FB: no framebuffer tag', 0
align 4
fp$mb2_pow10:
	dd 1000000000
	dd 100000000
	dd 10000000
	dd 1000000
	dd 100000
	dd 10000
	dd 1000
	dd 100
	dd 10
	dd 1
	dd 0				; sentinel
}

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
	; Multiboot2: EBX points at the boot-information structure. Copy the
	; framebuffer tag (if present) into the display mode block now, before
	; serial init clobbers EAX and before the long-mode switch. The copy and
	; the diagnostic run only when the image carries a display (the top-level
	; image sets FP_HAS_DISPLAY = 1 and provides fp$display_mode_block; the
	; bare OS core leaves it 0 and stays standalone).
	if FP_HAS_DISPLAY
	FP_MB2_COPY_FB
	end if
	jmp	.magic_ok
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

	; Serial is up. Report the framebuffer handoff so the physical T440 boot
	; shows exactly what the loader passed us. Only when the image carries a
	; display (the top-level image sets FP_HAS_DISPLAY = 1).
	if FP_HAS_DISPLAY
	FP_MB2_FB_DIAG
	end if

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
	; A correct 4 GB identity map needs only ONE PML4 entry and ONE PDPT,
	; because the whole 4 GB fits under a single PML4 slot (PML4[0] spans
	; 512 GB). The four 1 GB regions live in PDPT0[0..3], each pointing at
	; one PD of 512 x 2 MB large pages:
	;   PML4[0]    -> PDPT0
	;   PDPT0[0..3]-> PD0..PD3   (PDn covers [n GB, (n+1) GB))
	; The previous layout (PML4[n]->PDPTn, PDPTn[0]->PDn) only mapped the
	; first 1 GB correctly; addresses above 1 GB walked into an empty
	; PDPT0 slot and page-faulted. That was the framebuffer bug: the LFB at
	; 0xFD000000 (3.79 GB) was never actually mapped.
	; PML4[0] -> PDPT0 (present + writable)
	mov	edi, PML4_BASE
	mov	eax, PML4_BASE + 0x1000	; PDPT0
	or	eax, PTE_P_BIT or PTE_RW_BIT
	mov	[edi], eax
	mov	dword [edi + 4], 0	; high dword of the entry
	; PDPT0[0..3] -> PD0..3 (present + writable)
	mov	edi, PML4_BASE + 0x1000	; PDPT0
	mov	esi, PML4_BASE + 0x5000	; PD0
	mov	ecx, 4
      .pdpt:
	mov	eax, esi
	or	eax, PTE_P_BIT or PTE_RW_BIT
	mov	[edi], eax
	mov	dword [edi + 4], 0
	add	edi, 8			; next PDPT0 entry (8 bytes each)
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
