; =============================================================================
; FASM_PLUS T440 Layer — OS_TEST_T440.ASM
; T440 kernel image: the CPU-General OS Core plus the T440 hardware drivers
; (PCI enumeration, VESA display, PS/2 keyboard), assembled into one bootable
; binary.
;
; Build:
;   fasm OS_TEST_T440.ASM kernel_t440.bin        (run from the t440 directory)
; Boot in QEMU (display + serial + keyboard):
;   qemu-system-x86_64 -kernel kernel_t440.bin -serial stdio \
;       -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot
;   (omit -display none so the VGA window shows the framebuffer)
;
; Structure
; ---------
;   Region 2 (assembled first): the 64-bit OS Core (via OS_CORE.INC) followed
;     by the T440 driver code (PCI/VESA/PS2/init) — all use64.
;   Region 1: multiboot header + 32-bit entry stub (use32).
;   Region 2 (continued): the 64-bit entry point and the test runner.
;   Region 3: writable data (GDT/IDT images, boot GDT).
;   Region 4: zero-initialized BSS (stack, contexts, Cordis registry, and the
;     T440 driver state: PCI table, display mode block, keyboard buffer).
;
; Boot protocol: identical to the OS core — a 32-bit ELF with a Multiboot1
; header loaded at 0x100000. See os_core/README.md for why.
; =============================================================================

format elf executable at 0x100000
entry _start

; Select the boot protocol. 0 = Multiboot1 (QEMU -kernel, default for CI);
; 1 = Multiboot2 (GRUB2 on the physical T440). The build script passes the
; value; if it is not passed, default to Multiboot1 so the QEMU path works.
if ~ definite FP_MB2_HEADER
FP_MB2_HEADER = 0
end if

; The T440 image has a display. This tells the boot stub to copy the Multiboot2
; framebuffer tag into fp$display_mode_block and to print the framebuffer
; diagnostic to serial. The bare OS core leaves FP_HAS_DISPLAY = 0.
FP_HAS_DISPLAY = 1

; =============================================================================
; Region 0 (first): the Multiboot header, at the very start of the file.
; -----------------------------------------------------------------------------
; The loader scans only the first 8 KB of the image for the header, so it must
; precede the (large) 64-bit core. The header is emitted inline as raw bytes
; (it is a constant), so it needs no macro from OS_CORE.INC. `_start` — the
; ELF entry point — lives in the stub segment below and is resolved by FASM's
; multi-pass forward referencing.
; =============================================================================
segment readable executable
use32
  if FP_MB2_HEADER
	; Multiboot2 header: magic, arch, length, checksum, framebuffer request
	; tag (type 5), end tag. 8-byte aligned; must be in the first 8 KB.
align 8
fp$mb_header_start:
	dd 0xE85250D6			; Multiboot2 magic
	dd 0				; architecture: i386
	dd fp$mb_header_end - fp$mb_header_start
	dd -(0xE85250D6 + 0 + (fp$mb_header_end - fp$mb_header_start))
	; framebuffer request tag (type 5, flags 0, size 20)
align 8
	dw 5
	dw 0
	dd 20
	dd 1024				; preferred width
	dd 768				; preferred height
	dd 32				; preferred depth (bpp)
	; end tag
align 8
	dw 0
	dw 0
	dd 8
fp$mb_header_end:
  else
	; Multiboot1 header (QEMU -kernel). 4-byte aligned, in the first 8 KB.
align 4
fp$mb_header_start:
	dd 0x1BADB002			; Multiboot1 magic
	dd 0				; flags (ELF program headers describe the load)
	dd -(0x1BADB002 + 0)		; checksum
fp$mb_header_end:
  end if

; =============================================================================
; Region 2: 64-bit OS core + T440 driver code (use64)
; =============================================================================
use64
    include '../os_core/OS_CORE.INC'
    include 'PCI_ENUM.INC'
    include 'VESA_DISPLAY.INC'
    include 'PS2_KEYBOARD.INC'
    include 'T440_INIT.INC'
    ; include the OS core test functions but NOT its fp$run_all_tests (the
    ; T440 runner below supplies its own and adds the driver tests)
    FP_OS_TESTS_NO_RUNNER = 1
    include '../os_core/OS_TESTS.INC'

; =============================================================================
; Region 1 (continued): the 32-bit entry stub (use32)
; -----------------------------------------------------------------------------
; The stub macro is defined by BOOT_STUB.ASM (pulled in by OS_CORE.INC above),
; so it is available here. FP_MB2_HEADER defaults to 0, so the stub's magic
; check accepts both Multiboot1 and Multiboot2 entry magics.
; =============================================================================
segment readable executable
use32
    FP_BOOT_STUB_32

; =============================================================================
; Region 2 (continued): 64-bit entry point + the T440 test runner (use64)
; =============================================================================
segment readable executable
use64

; -----------------------------------------------------------------------------
; fp$long_mode_entry — 64-bit entry. Brings up the OS core, then the T440
; drivers, prints the heartbeat to BOTH serial and the screen, then runs the
; test suite. (We define it here, not via FP_LONG_MODE_ENTRY, because the T440
; kernel inserts the driver init between kernel_init and the test run.)
; -----------------------------------------------------------------------------
align 16
fp$long_mode_entry:
	mov	ax, GDT_KERNEL_DATA
	mov	ds, ax
	mov	es, ax
	mov	ss, ax
	mov	rsp, KERNEL_STACK_TOP
	and	rsp, -16
	; CPU-General core (serial, GDT, IDT, paging, exceptions, PIC, PIT,
	; syscalls, Cordis registry, boot banner)
	call	fp$kernel_init
	; T440 hardware drivers as Cordis fibers (PCI -> VESA -> Keyboard)
	call	fp$t440_init
	mov	[fp$t440_boot_result], rax	; driver failure bitmap (0 = all ok)
	; Heartbeat: '3' to serial AND to the screen.
	mov	rdi, '3'
	call	fp$serial_putchar
	mov	rdi, fp$heartbeat_nl
	call	fp$serial_write
	; screen heartbeat (top-left), white on black
	xor	rdi, rdi			; column 0
	xor	rsi, rsi			; row 0
	mov	rdx, '3'
	mov	rcx, 0x00FFFFFF			; white
	call	fp$vesa_putchar
	; run the test suite (12 core + 4 T440 driver tests; exits via 0xF4)
	call	fp$run_t440_tests
	; fp$run_t440_tests never returns; backstop halt
      .hang:
	cli
	hlt
	jmp	.hang

    ; --- The test runner (OS core tests + T440 driver tests) ---
    include 'OS_TESTS_T440.INC'

; =============================================================================
; Region 3: writable data (use64)
; =============================================================================
segment readable writeable
use64
    FP_GDT_IMAGE_SIZE = 8 * 8
    align 16
    fp$gdt_image	rb FP_GDT_IMAGE_SIZE
    align 16
    fp$idt_image	rb IDT_ENTRIES * IDT_ENTRY_SIZE
    align 16
    fp$boot_gdt:
	dq 0
	dw 0xFFFF, 0
	db 0, 0x9A, 0xAF, 0
	dw 0xFFFF, 0
	db 0, 0x92, 0x8F, 0
    fp$boot_gdt_end:
    align 16
    fp$boot_gdtr:
	dw fp$boot_gdt_end - fp$boot_gdt - 1
	dd fp$boot_gdt

    ; ---------------------------------------------------------------------
    ; Display mode block — lives in INITIALIZED DATA, not BSS.
    ;
    ; The 32-bit boot stub writes the Multiboot2 framebuffer tag here before
    ; the long-mode switch. If this block were in BSS, a loader that zeroes
    ; BSS at load time could wipe the values the stub just wrote (or the
    ; timing could race). Keeping it in .data (explicitly initialized to zero)
    ; means the loader writes real bytes to the file image and nothing
    ; re-zeroes it afterward. The stub then overwrites the fields when a
    ; framebuffer tag is present.
    ; ---------------------------------------------------------------------
    align 64
    fp$display_mode_block:
	dq 0				; lfb    (full 64-bit physical address)
	dq 0				; width
	dq 0				; height
	dq 0				; pitch
	dq 0				; bpp
    ; Multiboot2 framebuffer-handoff diagnostic strings + state (32-bit stub)
    FP_MB2_DATA

; =============================================================================
; Region 4: zero-initialized BSS (use64)
; =============================================================================
segment readable writeable
use64
    ; kernel stack
    FP_KERNEL_STACK_SIZE = 16384
    align 16
    fp$kernel_stack	rb FP_KERNEL_STACK_SIZE
    fp$kernel_stack_top:
    ; task contexts (Cordis self-test + general use)
    align 64
    fp$context_a	rb FP_CONTEXT_SIZE
    align 64
    fp$context_b	rb FP_CONTEXT_SIZE
    ; TSS
    align 16
    fp$tss		rb 104
    ; Cordis Context Registry
    align 64
    fp$cordis_fibers	rb CORDIS_MAX_FIBERS * CORDIS_FIBER_SIZE
    align 64
    fp$cordis_coeffects	rb CORDIS_MAX_COEFFECTS * CORDIS_COEFFECT_SIZE
    align 64
    fp$cordis_effects	rb CORDIS_MAX_EFFECTS * CORDIS_EFFECT_SIZE
    align 8
    fp$cordis_scope_fiber rq 1
    ; ----- T440 driver state -----
    ; PCI device table + count
    align 64
    fp$pci_device_table	rb PCI_MAX_DEVICES * PCI_DEV_SIZE
    align 8
    fp$pci_device_count rq 1
    ; (the display mode block moved to Region 3 / .data — see above)
    ; putchar scratch slots (glyph base + fg color survive fp$vesa_putpixel,
    ; which clobbers rcx/r8)
    align 8
    fp$vesa_glyph_ptr rq 1
    fp$vesa_fg_color rq 1
    ; keyboard ring buffer + indices + flags
    align 64
    fp$kbd_buffer	rb KBD_BUF_SIZE
    align 8
    fp$kbd_head	rd 1			; ring buffer write index (dword)
    fp$kbd_tail	rd 1			; ring buffer read index (dword)
    fp$kbd_break_pending rb 1		; 1 = next scancode is a break/extended
    ; T440 driver-boot result bitmap (0 = all drivers active)
    align 8
    fp$t440_boot_result rq 1
