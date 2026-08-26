; =============================================================================
; FASM_PLUS OS Core — OS_TEST.ASM
; Main kernel image: assembles the whole OS core into a bootable binary.
;
; Build:
;   fasm OS_TEST.ASM kernel.bin
; Boot (heartbeat — expect a single '3' on serial):
;   qemu-system-x86_64 -kernel kernel.bin -serial stdio -display none -no-reboot
; Full integration suite (8 tests, exits via isa-debug-exit):
;   qemu-system-x86_64 -kernel kernel.bin -serial stdio -display none \
;       -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot
;
; Structure (one ELF image, four loadable regions)
; ------------------------------------------------
;   1. multiboot header + 32-bit entry stub   (use32, at the file start)
;   2. 64-bit kernel code                     (use64)
;   3. writable data (GDT/IDT images, strings, test state)
;   4. zero-initialized BSS (stack, task contexts, syscall table area)
;
; Boot protocol
; -------------
; The OS core is written to the Multiboot2 specification. The CI emulator
; (QEMU 8.2) only loads `-kernel` images that are 32-bit ELF carrying an
; original Multiboot (v1) header — it rejects ELF64-class images and PVH ELF
; notes. This image is therefore assembled as a 32-bit ELF with a Multiboot1
; header so `qemu-system-x86_64 -kernel` boots it directly. BOOT_STUB.ASM
; accepts both the Multiboot1 and Multiboot2 entry magics, and defining
; FP_MB2_HEADER = 1 (before OS_CORE.INC) switches the emitted header to
; Multiboot2 for a v2 loader such as GRUB2. The kernel proper is pure 64-bit
; long-mode code; the ELF class constrains only the container, not the
; instruction encodings (the REX.W-patched FASM encodes both 32- and 64-bit).
; =============================================================================

; Load the kernel at 1 MB, the conventional protected-mode kernel base. The
; identity map built by the boot stub covers this address, so the ELF virtual
; and physical addresses coincide.
format elf executable at 0x100000
entry _start

; =============================================================================
; Region 2 (assembled first): 64-bit kernel code (use64)
; -----------------------------------------------------------------------------
; OS_CORE.INC assembles every OS core function body (serial, GDT, IDT, paging,
; exceptions, context, syscall, init, test) as 64-bit code. It is included
; before the stub segment because the stub's macros (FP_MULTIBOOT_HEADER,
; FP_BOOT_STUB_32, FP_LONG_MODE_ENTRY) and constants are defined by it, and
; FASM requires a macro to be defined before it is invoked. The multiboot
; header still lands within the first 8 KB of the image (the loader scans
; that window), and `entry _start` points the ELF at the 32-bit stub.
; =============================================================================
use64
    include 'OS_CORE.INC'

; =============================================================================
; Region 1: multiboot header + 32-bit entry stub (use32)
; -----------------------------------------------------------------------------
; =============================================================================
segment readable executable
use32

    ; --- Multiboot header (must be within the first 8 KB) ---
    FP_MULTIBOOT_HEADER

    ; --- 32-bit entry point: verify magic, init COM1, build page tables,
    ;     enable PAE + long mode + paging, load the boot GDT, far-jump to
    ;     fp$long_mode_entry. On failure print a one-letter code and halt. ---
    FP_BOOT_STUB_32

; =============================================================================
; Region 2 (continued): 64-bit entry point and the test runner (use64)
; =============================================================================
segment readable executable
use64

    ; --- 64-bit entry: set segments and stack, init the kernel, run tests ---
    FP_LONG_MODE_ENTRY

    ; --- The test runner (heartbeat + the 8 integration tests) ---
    include 'OS_TESTS.INC'

; =============================================================================
; Region 3: writable data (use64)
; -----------------------------------------------------------------------------
; Shared between the 32-bit stub and the 64-bit kernel. Holds the GDT image,
; the IDT image, the boot-time GDTR, and the syscall dispatch table area.
; =============================================================================
segment readable writeable
use64

    ; ----- GDT image: 6 slots used (null, kcode, kdata, ucode, udata, TSS);
    ;       the TSS descriptor spans slots 5-6 (16 bytes), so reserve 8. -----
    FP_GDT_IMAGE_SIZE = 8 * 8         ; 8 slots x 8 bytes
    align 16
    fp$gdt_image	rb FP_GDT_IMAGE_SIZE

    ; ----- IDT image: 256 entries x 16 bytes = 4 KB -----
    align 16
    fp$idt_image	rb IDT_ENTRIES * IDT_ENTRY_SIZE

    ; ----- Boot GDT + GDTR used by the 32-bit stub before the full GDT is
    ;       built by fp$gdt_init. Entries match the selector layout in
    ;       GDT.INC so the long-mode switch is seamless. -----
    align 16
    fp$boot_gdt:
	dq 0					; 0x00 null
	; 0x08 kernel code: limit 0xFFFF, base 0, access 0x9A (P,E,RW),
	; gran 0xAF (G, L, limit high 0xF)
	dw 0xFFFF, 0
	db 0, 0x9A, 0xAF, 0
	; 0x10 kernel data: access 0x92 (P,RW), gran 0x8F (G, limit high 0xF)
	dw 0xFFFF, 0
	db 0, 0x92, 0x8F, 0
    fp$boot_gdt_end:
    align 16
    fp$boot_gdtr:
	dw fp$boot_gdt_end - fp$boot_gdt - 1	; limit
	dd fp$boot_gdt				; base (32-bit stub addressing)

; =============================================================================
; Region 4: zero-initialized BSS (use64)
; -----------------------------------------------------------------------------
; The kernel stack, the task-switcher context structures, and the syscall
; dispatch table live here. This region occupies no space in the file.
; =============================================================================
segment readable writeable
use64

    ; ----- Kernel stack: 16 KB, top at KERNEL_STACK_TOP (0x70000) is provided
    ;       by the fixed memory map; this is an additional guard/test stack. -----
    FP_KERNEL_STACK_SIZE = 16384
    align 16
    fp$kernel_stack	rb FP_KERNEL_STACK_SIZE
    fp$kernel_stack_top:

    ; ----- Task context structures (64-byte aligned, FP_CONTEXT_SIZE each) ---
    align 64
    fp$context_a	rb FP_CONTEXT_SIZE
    align 64
    fp$context_b	rb FP_CONTEXT_SIZE

    ; ----- Task State Segment (104 bytes) for ring transitions -----
    align 16
    fp$tss		rb 104
