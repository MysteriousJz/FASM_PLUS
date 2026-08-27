; =============================================================================
; FASM_PLUS T440 Layer — VESA_DISPLAY.INC
; Linear-framebuffer display driver: draw pixels, characters, and strings to
; the screen.
;
; Depends on: CORDIS.INC (this driver is a Cordis fiber), PCI_ENUM.INC
; Emits: functions in the current code section; a small state block in BSS;
;        the 8x16 font is read-only data.
;
; THE FRAMEBUFFER PROBLEM (read this first)
; -----------------------------------------
; Setting a VBE mode requires the BIOS (int 0x10), which only exists in real
; mode. The kernel runs in 64-bit long mode, where int 0x10 is unavailable and
; there is no VM86 monitor. So the mode is established BEFORE the kernel takes
; over, and this driver is handed the resulting linear frame buffer (LFB):
;
;   * QEMU (-kernel): QEMU's multiboot loader does NOT set a graphics mode,
;     but the emulated VGA exposes the Bochs Graphics Adapter (BGA) VBE
;     interface on I/O ports 0x1CE/0x1CF. That interface is pure port I/O and
;     works from long mode with no BIOS, so the QEMU path programs the BGA
;     directly and reads the LFB physical address from the VGA device's BAR0
;     (found via the PCI table).
;
;   * ThinkPad T440 (real hardware): a Multiboot2 loader such as GRUB2 sets
;     the VBE mode and hands the kernel a framebuffer tag. The stub copies the
;     tag into fp$display_mode_block before the long-mode switch; this driver
;     then uses it directly. (See T440_INIT.INC / the boot stub.)
;
; Either way, the driver works from a normalized mode block:
;   fp$display_mode_block = { lfb_addr, width, height, pitch, bpp }
;
; Cordis integration
; ------------------
;   Fiber name : VESA_DISPLAY (fiber id T440_FIBER_VESA)
;   Provides   : DISPLAY_FRAMEBUFFER (value = LFB base address)
;   Injects    : PCI_DEVICE_TABLE (to locate the VGA device BAR)
;   Effects    : enabling the BGA mode is revertible (inverse: disable BGA,
;                returning the emulated card to text mode). On real hardware
;                the mode was set by the bootloader, so the inverse restores
;                nothing the kernel changed — it simply clears the provision.
;
; Success criterion: the kernel prints '3' to the top-left of the screen.
; =============================================================================

if ~ definite FP_VESA_DISPLAY_INC
FP_VESA_DISPLAY_INC = 1

if ~ definite FP_PCI_ENUM_INC
  err 'VESA_DISPLAY.INC requires PCI_ENUM.INC (and CORDIS.INC) first'
end if

; -----------------------------------------------------------------------------
; Bochs BGA / VBE dispi ports and registers (QEMU std VGA + BGA)
; -----------------------------------------------------------------------------
BGA_INDEX_PORT   = 0x1CE
BGA_DATA_PORT    = 0x1CF
BGA_IDX_ID       = 0x00
BGA_IDX_XRES     = 0x01
BGA_IDX_YRES     = 0x02
BGA_IDX_BPP      = 0x03
BGA_IDX_ENABLE   = 0x04
BGA_IDX_LFB      = 0x0A        ; read: LFB physical address (dword via two reads)
BGA_ENABLE       = 0x01
BGA_LFB_ENABLED  = 0x40
BGA_ID_MIN       = 0xB0C0      ; any ID in 0xB0C0..0xB0C5 means BGA present

; default mode the driver tries to establish (QEMU path)
VESA_TARGET_X    = 1024
VESA_TARGET_Y    = 768
VESA_TARGET_BPP  = 32

; PCI class for a VGA-compatible display controller (class 0x03, subclass 0x00)
PCI_CLASS_DISPLAY = 0x03
PCI_SUBCLASS_VGA  = 0x00

; Coeffect key this fiber provides; value = LFB base address.
COEFFECT_DISPLAY_FRAMEBUFFER = 0x7002

; text cell geometry (8x16 font)
VESA_FONT_W      = 8
VESA_FONT_H      = 16

; =============================================================================
; fp$bga_write — write a BGA dispi register
;   CONTRACT: IN di = index, si = value; CLOBBERS rax, rdx, flags.
; =============================================================================
align 16
fp$bga_write:
	mov	dx, BGA_INDEX_PORT
	movzx	eax, di
	out	dx, ax
	mov	dx, BGA_DATA_PORT
	movzx	eax, si
	out	dx, ax
	ret

; =============================================================================
; fp$bga_read — read a BGA dispi register
;   CONTRACT: IN di = index; OUT ax = value; CLOBBERS rax, rdx, flags.
; =============================================================================
align 16
fp$bga_read:
	mov	dx, BGA_INDEX_PORT
	movzx	eax, di
	out	dx, ax
	mov	dx, BGA_DATA_PORT
	in	ax, dx
	ret

; =============================================================================
; fp$vesa_detect — fill fp$display_mode_block with a usable framebuffer
;   CONTRACT
;     IN        : nothing
;     OUT       : rax = 0 on success (mode block valid), -1 if no framebuffer
;     CLOBBERS  : rax, rcx, rdx, rsi, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15
;     ERRORS    : -1 when neither a bootloader LFB nor a BGA device exists
;   Order of attempts:
;     1. If the boot stub already stored a nonzero LFB (bootloader-provided
;        framebuffer, the T440 path), adopt it.
;     2. Else program the Bochs BGA (QEMU path): set the target mode with the
;        LFB enabled and read the LFB base from the VGA device's PCI BAR0.
; =============================================================================
align 16
fp$vesa_detect:
	; path 1: bootloader-provided framebuffer already present?
	mov	rax, qword [fp$display_mode_block + display_mode.lfb]
	test	rax, rax
	jnz	.adopt			; nonzero LFB -> use it (T440 path)
	; path 2: Bochs BGA (QEMU). Check the ID register.
	mov	di, BGA_IDX_ID
	call	fp$bga_read
	cmp	ax, BGA_ID_MIN
	jb	.fail			; below 0xB0C0 -> no BGA
	; program the target mode: disable, set X/Y/BPP, enable with LFB
	mov	di, BGA_IDX_ENABLE
	xor	esi, esi		; disable
	call	fp$bga_write
	mov	di, BGA_IDX_XRES
	mov	esi, VESA_TARGET_X
	call	fp$bga_write
	mov	di, BGA_IDX_YRES
	mov	esi, VESA_TARGET_Y
	call	fp$bga_write
	mov	di, BGA_IDX_BPP
	mov	esi, VESA_TARGET_BPP
	call	fp$bga_write
	mov	di, BGA_IDX_ENABLE
	mov	esi, BGA_ENABLE or BGA_LFB_ENABLED
	call	fp$bga_write
	; find the VGA device and read BAR0 (the LFB physical address)
	mov	rdi, PCI_CLASS_DISPLAY
	mov	rsi, PCI_SUBCLASS_VGA
	call	fp$pci_find_class
	cmp	rax, -1
	je	.fail
	; slot -> table entry
	mov	rcx, PCI_DEV_SIZE
	imul	rax, rcx
	add	rax, fp$pci_device_table
	; enable the device's memory space (and bus mastering) or its BAR-backed
	; framebuffer is not mapped and any LFB write faults. The table entry
	; carries bus/device/function for exactly this.
	push	rax
	mov	rdi, [rax + pci_dev.bus]
	mov	rsi, [rax + pci_dev.device]
	mov	rdx, [rax + pci_dev.function]
	call	fp$pci_enable_device
	pop	rax
	mov	rcx, [rax + pci_dev.bar0]
	and	ecx, 0xFFFFFFF0		; mask BAR flags -> memory base (32-bit op)
	; store the mode block
	mov	qword [fp$display_mode_block + display_mode.lfb], rcx
	mov	rax, VESA_TARGET_X
	mov	qword [fp$display_mode_block + display_mode.width], rax
	mov	rax, VESA_TARGET_Y
	mov	qword [fp$display_mode_block + display_mode.height], rax
	mov	rax, VESA_TARGET_X * 4	; pitch = width * (bpp/8) = width*4
	mov	qword [fp$display_mode_block + display_mode.pitch], rax
	mov	rax, VESA_TARGET_BPP
	mov	qword [fp$display_mode_block + display_mode.bpp], rax
	xor	eax, eax
	ret
      .adopt:
	xor	eax, eax		; mode block already valid
	ret
      .fail:
	mov	rax, -1
	ret

; =============================================================================
; fp$vesa_putpixel — draw one pixel
;   CONTRACT
;     IN        : rdi = x, rsi = y, rdx = color (0x00RRGGBB)
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, r8, flags
;     PRESERVES : rbx, rbp, rsi, rdi, r9-r15
;     ERRORS    : silently clips out-of-range coordinates
;   Pixel offset = y * pitch + x * (bpp/8). 32-bpp uses a single dword store.
; =============================================================================
align 16
fp$vesa_putpixel:
	mov	rax, qword [fp$display_mode_block + display_mode.lfb]
	test	rax, rax
	jz	.out			; no framebuffer -> drop the pixel
	; The framebuffer's physical range is mapped into the page tables before
	; the driver activates (fp$paging_map_range), so any LFB address is
	; writable. No address guard is needed here.
	; bounds check
	cmp	rdi, qword [fp$display_mode_block + display_mode.width]
	jae	.out
	cmp	rsi, qword [fp$display_mode_block + display_mode.height]
	jae	.out
	; offset = y*pitch + x*4
	mov	rcx, rsi
	imul	rcx, qword [fp$display_mode_block + display_mode.pitch]
	mov	r8, rdi
	shl	r8, 2			; x*4 (32 bpp; shift, no mul)
	add	rcx, r8
	add	rcx, rax		; rcx = pixel address
	mov	[rcx], edx		; store the pixel
      .out:
	ret

; =============================================================================
; fp$vesa_clear — fill the whole framebuffer with a color
;   CONTRACT
;     IN        : rdi = color (0x00RRGGBB)
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, rsi, r8-r15
;     ERRORS    : none (no-op if there is no framebuffer)
;   Fills pitch*height bytes as dwords. The byte count is pitch*height, and
;   the loop count is that divided by 4 — pitch is width*4, so pitch*height/4
;   = width*height, which we compute with one imul (no division).
; =============================================================================
align 16
fp$vesa_clear:
	mov	rcx, qword [fp$display_mode_block + display_mode.lfb]
	test	rcx, rcx
	jz	.out
	; The framebuffer is mapped before the driver activates, so the fill runs
	; regardless of the LFB address.
	mov	rax, qword [fp$display_mode_block + display_mode.width]
	mov	rdx, qword [fp$display_mode_block + display_mode.height]
	imul	rax, rdx		; rax = pixel count = dwords to write
	mov	rcx, qword [fp$display_mode_block + display_mode.lfb]
      .fill:
	mov	[rcx], edi
	add	rcx, 4
	dec	rax
	jnz	.fill
      .out:
	ret

; =============================================================================
; fp$vesa_putchar — draw an 8x16 character at a text cell
;   CONTRACT
;     IN        : rdi = column (text cell x), rsi = row (text cell y),
;                 rdx = ASCII character, rcx = foreground color (0x00RRGGBB)
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rsi, rdi, r8-r11, flags
;     PRESERVES : rbx, rbp, r12-r15
;     ERRORS    : none (unknown glyphs render as a filled box)
;   Renders one glyph from the 8x16 font table. Each of the 16 glyph rows is
;   one byte; bit 7 is the leftmost pixel. Foreground bits get the color,
;   clear bits get the background (black).
; =============================================================================
align 16
fp$vesa_putchar:
	push	rbx
	push	r12
	push	r13
	push	r14
	push	r15
	; fp$vesa_putpixel clobbers rcx and r8, so the two values we need across
	; every pixel call — the glyph base pointer and the foreground color —
	; are parked in BSS slots before the loop.
	mov	[fp$vesa_fg_color], rcx		; fg color (in)
	; glyph base = font + char*16
	and	rdx, 0xFF			; char
	mov	rax, rdx
	shl	rax, 4				; char*16 (shift, no mul)
	lea	rcx, [fp$font8x16]
	add	rcx, rax
	mov	[fp$vesa_glyph_ptr], rcx	; &glyph[0]
	; pixel origin
	mov	r12, rdi
	shl	r12, 3				; x = col*8
	mov	r13, rsi
	shl	r13, 4				; y = row*16
	xor	r15, r15			; r15 = glyph row (0..15)
      .row:
	cmp	r15, VESA_FONT_H
	jae	.done
	mov	rbx, [fp$vesa_glyph_ptr]
	movzx	ebx, byte [rbx + r15]		; row bitmask
	xor	r14, r14			; r14 = column bit (0..7)
      .col:
	cmp	r14, VESA_FONT_W
	jae	.nextrow
	; test bit (7 - col) of the row bitmask
	mov	ecx, 7
	sub	ecx, r14d
	bt	ebx, ecx
	jnc	.pixel_bg
	mov	rdx, [fp$vesa_fg_color]		; foreground
	jmp	.plot
      .pixel_bg:
	xor	edx, edx			; background = black
      .plot:
	lea	rdi, [r12 + r14]		; x = origin_x + col
	lea	rsi, [r13 + r15]		; y = origin_y + row
	call	fp$vesa_putpixel		; preserves rbx, r12-r15
	inc	r14
	jmp	.col
      .nextrow:
	inc	r15
	jmp	.row
      .done:
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret

; =============================================================================
; fp$vesa_print — print a null-terminated string at a text position
;   CONTRACT
;     IN        : rdi = column, rsi = row, rdx = string pointer,
;                 rcx = foreground color
;     OUT       : rax = number of characters printed
;     CLOBBERS  : rax, rcx, rdx, rsi, rdi, r8, flags
;     PRESERVES : rbx, rbp, r12-r15
;     ERRORS    : none
;   Advances the column per character and wraps at the screen's text width
;   (width/8); '\n' moves to the next row. No division: the wrap column is
;   width shr 3 (width is a multiple of 8 for all modes we use).
; =============================================================================
align 16
fp$vesa_print:
	push	rbx
	push	r12
	push	r13
	push	r14
	mov	r12, rdi		; r12 = current column
	mov	r13, rsi		; r13 = current row
	mov	r14, rdx		; r14 = string cursor
	mov	r8, rcx			; r8  = color
	xor	r9, r9			; r9  = count
	; wrap column = width/8
	mov	rbx, qword [fp$display_mode_block + display_mode.width]
	shr	rbx, 3
      .loop:
	movzx	eax, byte [r14]
	test	al, al
	jz	.done
	cmp	al, 10			; newline
	je	.newline
	; draw the character
	mov	rdi, r12
	mov	rsi, r13
	movzx	edx, al
	mov	rcx, r8
	call	fp$vesa_putchar
	inc	r12
	cmp	r12, rbx		; wrap?
	jb	.advanced
      .newline:
	xor	r12, r12
	inc	r13
      .advanced:
	inc	r14
	inc	r9
	jmp	.loop
      .done:
	mov	rax, r9
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret

; =============================================================================
; fp$vesa_apply — the Cordis fiber's apply function
;   CONTRACT
;     IN        : nothing (fiber id is the constant T440_FIBER_VESA)
;     OUT       : rax = 0 on success, -1 if no framebuffer could be found
;     CLOBBERS  : all
;     ERRORS    : -1 when no framebuffer exists (then the provision is not made)
;   Detects/establishes the framebuffer, records the revertible effect, and
;   provides DISPLAY_FRAMEBUFFER with the LFB base as its value.
; =============================================================================
align 16
fp$vesa_apply:
	; record the revertible effect first (LIFO keeps ordering safe)
	mov	rdi, T440_FIBER_VESA
	mov	rsi, fp$vesa_inverse
	xor	rdx, rdx
	call	fp$effect_record
	test	rax, rax
	js	.fail
	; establish the framebuffer (fills fp$display_mode_block). By now the
	; LFB address is known — from the Multiboot2 framebuffer tag (T440) or
	; from the Bochs BGA + PCI BAR (QEMU).
	call	fp$vesa_detect
	test	rax, rax
	jnz	.fail
	; Map the framebuffer's physical range into the page tables so the CPU
	; can write it. The byte count is pitch * height (the pitch may exceed
	; width * bytes-per-pixel). This runs AFTER the address is known and
	; BEFORE any drawing.
	mov	rdi, qword [fp$display_mode_block + display_mode.lfb]
	mov	rax, qword [fp$display_mode_block + display_mode.pitch]
	mov	rcx, qword [fp$display_mode_block + display_mode.height]
	imul	rax, rcx		; rax = pitch * height = framebuffer bytes
	mov	rsi, rax
	call	fp$paging_map_range
	; flush the TLB so the new framebuffer mappings take effect immediately
	mov	rax, cr3
	mov	cr3, rax
	; clear the screen to black so the heartbeat is readable
	xor	rdi, rdi
	call	fp$vesa_clear
	; provide the framebuffer base to dependents
	mov	rdi, T440_FIBER_VESA
	mov	rsi, COEFFECT_DISPLAY_FRAMEBUFFER
	mov	rdx, qword [fp$display_mode_block + display_mode.lfb]
	call	fp$coeffect_provide
	test	rax, rax
	js	.fail
	xor	eax, eax
	ret
      .fail:
	mov	rax, -1
	ret

; fp$vesa_inverse — revert the display effect (disable BGA / clear provision).
;   CONTRACT: IN rdi = context (ignored); CLOBBERS rax, rdx, rsi, flags.
align 16
fp$vesa_inverse:
	; disable the BGA (QEMU path). Harmless on real hardware, whose mode was
	; set by the bootloader (the ports are simply absent).
	mov	di, BGA_IDX_ENABLE
	xor	esi, esi
	call	fp$bga_write
	; clear the mode block so dependents see no framebuffer
	xor	eax, eax
	mov	qword [fp$display_mode_block + display_mode.lfb], rax
	ret

; -----------------------------------------------------------------------------
; The normalized mode block (populated by the boot stub or fp$vesa_detect).
; -----------------------------------------------------------------------------
struc display_mode {
    .lfb    dq ?                  ; linear frame buffer physical/virtual base
    .width  dq ?                  ; pixels
    .height dq ?                  ; pixels
    .pitch  dq ?                  ; bytes per scanline
    .bpp    dq ?                  ; bits per pixel
}
virtual at 0
    display_mode display_mode
    display_mode_end:
end virtual
DISPLAY_MODE_SIZE = display_mode_end

; =============================================================================
; 8x16 bitmap font, glyphs 32..126 (printable ASCII). Each glyph is 16 bytes,
; bit 7 = leftmost pixel. Generated from the classic VGA 8x16 font.
; Index 0 in the table corresponds to ASCII 32 (space); the driver masks the
; character to the table range before lookup.
; =============================================================================
align 16
fp$font8x16:
    ; This table holds glyphs for ASCII 0..127 (128 * 16 bytes). Glyphs 0..31
    ; are blank; 32..126 are printable; 127 is blank. Filled below.
    include 'FONT8X16.INC'

end if ; FP_VESA_DISPLAY_INC
