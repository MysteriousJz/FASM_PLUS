; =============================================================================
; FASM_PLUS Foundation Library — EXAMPLE.ASM
; A small program demonstrating the foundation layers working together:
; bootstrap, crash-handler installation, memory allocation, structured
; control flow, and formatted I/O.
;
; Build:  fasm/fasm.x64 foundation/EXAMPLE.ASM example
; Run:    ./example
; =============================================================================

format elf64 executable
entry _start

include 'CONSTANTS.INC'
include 'CONTRACTS.INC'
include 'STATE_MANAGEMENT.INC'
include 'MEMORY_PRIMITIVES.INC'
include 'CONTROL_FLOW.INC'
include 'IO_BASIC.INC'
include 'TEST_FRAMEWORK.INC'
include 'BOOTSTRAP.INC'
include 'CRASH_DETECTOR.INC'

segment readable executable
_start:
	; initialize the platform and arm crash detection
	call	fp$init
	cmp	rax, E_OK
	jne	.fail
	call	fp$crash_install
	cmp	rax, E_OK
	jne	.fail

	lea	rsi, [ex$banner]
	call	fp$print_cstr

	; report detected CPU features
	lea	rsi, [ex$cpu]
	call	fp$print_cstr
	mov	rdi, [fp$cpu_features]
	call	fp$print_hex
	call	fp$print_newline

	; set up a bump allocator over a static arena
	lea	rdi, [ex$arena]
	mov	rsi, 512
	call	fp$alloc_init
	cmp	rax, E_OK
	jne	.fail

	; allocate a small buffer and fill it with a pattern using a FOR loop
	mov	rdi, 64
	call	fp$alloc
	cmp	rax, 0
	jle	.fail
	mov	rbx, rax			; rbx = our buffer

	lea	rsi, [ex$filling]
	call	fp$print_cstr

	xor	r13, r13			; checksum
	FOR r12, 0, 64, 1, .fill
	mov	rax, r12
	imul	rax, 7				; a trivial pattern
	mov	[rbx+r12], al
	add	r13, rax			; accumulate
	NEXT r12, 1, .fill

	; report the checksum (low 32 bits for readability)
	lea	rsi, [ex$checksum]
	call	fp$print_cstr
	mov	rdi, r13
	call	fp$print_uint
	call	fp$print_newline

	; demonstrate the guard-page allocator: map 1 page + guard, touch the
	; usable page, and report success (we deliberately do NOT touch the
	; guard page here — the smoke tests and crash detector cover faults)
	mov	rdi, 1
	call	fp$guard_map
	cmp	rax, 0
	jle	.fail
	mov	r14, rax			; base of guarded region
	mov	byte [r14], 0xAA		; touch the first usable byte

	lea	rsi, [ex$guardok]
	call	fp$print_cstr

	; unmap the guarded region (1 usable + 1 guard page)
	mov	rdi, r14
	mov	rsi, 2 * PAGE_SIZE
	call	fp$munmap

	lea	rsi, [ex$done]
	call	fp$print_cstr
	mov	edi, EXIT_SUCCESS
	call	fp$exit

.fail:
	lea	rsi, [ex$fail]
	call	fp$halt_error

segment readable writeable
ex$banner	db 'FASM_PLUS foundation example',10,0
ex$cpu		db 'cpu features: 0x',0
ex$filling	db 'filled 64-byte arena buffer with pattern',10,0
ex$checksum	db 'checksum: ',0
ex$guardok	db 'guard-mapped page touched and unmapped cleanly',10,0
ex$done		db 'example complete',10,0
ex$fail		db 'example failed',10,0
align 16
ex$arena	rb 512

include 'FOUNDATION_DATA.INC'

; the example defines no tests, but the registry symbols must exist because
; TEST_FRAMEWORK/BOOTSTRAP reference them
segment readable writeable
align 8
fp$test_registry_begin:
fp$test_registry_end:
FP_TEST_TOTAL = 0
