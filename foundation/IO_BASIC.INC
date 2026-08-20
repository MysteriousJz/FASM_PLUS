; =============================================================================
; FASM_PLUS Foundation Library — Layer 4
; IO_BASIC.INC — console and file I/O built directly on Linux syscalls
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, MEMORY_PRIMITIVES.INC (for
;             fp$mem_* where noted)
; Emits: functions in the current code section; no data.
;
; No libc, no buffering surprises: every function maps to one or two syscalls
; and returns the kernel's result. Output functions return the byte count in
; rax (or a negative errno). Integer formatting is done into a caller-visible
; scratch buffer to stay reentrant.
; =============================================================================

if ~ definite FP_IO_BASIC_INC
FP_IO_BASIC_INC = 1

if ~ definite FP_MEMORY_PRIMITIVES_INC
  err 'IO_BASIC.INC requires MEMORY_PRIMITIVES.INC first'
end if

; =============================================================================
; fp$write — raw write to a file descriptor
;   CONTRACT
;     IN        : rdi = fd, rsi = buffer, rdx = byte count
;     OUT       : rax = bytes written, or negative errno
;     CLOBBERS  : rax rcx r11 flags (syscall clobbers rcx/r11)
;     PRESERVES : rdi rsi rdx and all non-volatile registers
;     ERRORS    : negative errno from the kernel
; =============================================================================
align 16
fp$write:
	mov	eax, SYS_write
	syscall
	ret

; =============================================================================
; fp$print_char — write one character to stdout
;   CONTRACT
;     IN        : dil = character
;     OUT       : rax = 1 on success, or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r11 flags; uses the stack red zone for
;                 the one-byte buffer (safe: this is a leaf except syscall)
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$print_char:
	; store the byte into the red zone (128 bytes below rsp are ours to use)
	mov	[rsp-8], dil
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	ret

; =============================================================================
; fp$print — write a counted string to stdout
;   CONTRACT
;     IN        : rsi = buffer, rdx = byte count
;     OUT       : rax = bytes written, or negative errno
;     CLOBBERS  : rax rcx rdi r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$print:
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	syscall
	ret

; =============================================================================
; fp$print_cstr — write a NUL-terminated string to stdout
;   CONTRACT
;     IN        : rsi = NUL-terminated string
;     OUT       : rax = bytes written (excluding the NUL), or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$print_cstr:
	; compute length with a scan
	mov	rdx, -1
.len:
	inc	rdx
	cmp	byte [rsi+rdx], 0
	jne	.len
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	syscall
	ret

; =============================================================================
; fp$print_newline — write a single newline to stdout
;   CONTRACT: IN none; OUT rax = 1 or negative errno; CLOBBERS rax rcx rdx
;             rsi rdi r11 flags
; =============================================================================
align 16
fp$print_newline:
	mov	byte [rsp-8], 10
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	ret

; =============================================================================
; fp$print_uint — print an unsigned 64-bit integer in decimal
;   CONTRACT
;     IN        : rdi = value
;     OUT       : rax = characters written, or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r8 r9 r10 r11 flags
;                 uses a 32-byte scratch on the stack (red zone safe)
;     ERRORS    : negative errno from write
; =============================================================================
align 16
fp$print_uint:
	; build digits backward into the red zone, then write forward
	lea	r9, [rsp-8]		; write pointer, grows downward
	mov	rax, rdi
	mov	r8, 10
	xor	ecx, ecx		; digit count
.divloop:
	xor	edx, edx
	div	r8			; rax = quot, rdx = rem
	add	dl, '0'
	dec	r9
	mov	[r9], dl
	inc	rcx
	test	rax, rax
	jnz	.divloop
	; write rcx bytes from r9
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	mov	rsi, r9
	mov	rdx, rcx
	syscall
	ret

; =============================================================================
; fp$print_int — print a signed 64-bit integer in decimal
;   CONTRACT
;     IN        : rdi = signed value
;     OUT       : rax = characters written (including '-'), or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r8-r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$print_int:
	test	rdi, rdi
	jns	fp$print_uint		; non-negative: defer to unsigned
	; emit '-' then print the magnitude. Save the value across the call.
	push	rdi
	mov	dil, '-'
	call	fp$print_char
	pop	rdi
	; careful: INT64_MIN cannot be negated in range; neg of INT64_MIN is
	; itself, but div on it as unsigned gives the right digit sequence
	; because we print the two's-complement magnitude.
	neg	rdi
	jmp	fp$print_uint

; =============================================================================
; fp$print_hex — print a 64-bit value as 16 lowercase hex digits
;   CONTRACT
;     IN        : rdi = value
;     OUT       : rax = 16 (chars written), or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r8 r9 r11 flags
;     ERRORS    : negative errno
;   Fixed width keeps output aligned and diff-friendly in test logs.
; =============================================================================
align 16
fp$print_hex:
	lea	r9, [rsp-24]		; 16-byte scratch in the red zone
	mov	ecx, 16			; digit count
.digit:
	mov	rax, rdi
	and	rax, MASK_LOW_NIBBLE
	cmp	al, 10
	jb	.num
	add	al, 'a'-10
	jmp	.store
.num:
	add	al, '0'
.store:
	dec	r9
	mov	[r9], al
	shr	rdi, 4
	dec	ecx
	jnz	.digit
	mov	eax, SYS_write
	mov	edi, STDOUT_FD
	mov	rsi, r9
	mov	edx, 16
	syscall
	ret

; =============================================================================
; fp$read_char — read one character from stdin (blocking)
;   CONTRACT
;     IN        : none
;     OUT       : rax = character (0-255) on success, 0 on EOF, negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$read_char:
	mov	eax, SYS_read
	mov	edi, STDIN_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	cmp	rax, 0
	jle	.done			; 0 = EOF, negative = errno
	movzx	eax, byte [rsp-8]
.done:
	ret

; =============================================================================
; fp$file_open — open a file (openat with AT_FDCWD semantics via open)
;   CONTRACT
;     IN        : rdi = NUL-terminated path, rsi = flags (O_*), rdx = mode
;     OUT       : rax = file descriptor, or negative errno
;     CLOBBERS  : rax rcx r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$file_open:
	mov	eax, SYS_open
	syscall
	ret

; =============================================================================
; fp$file_read / fp$file_write — read/write up to count bytes
;   CONTRACT
;     IN        : rdi = fd, rsi = buffer, rdx = count
;     OUT       : rax = bytes transferred, or negative errno
;     CLOBBERS  : rax rcx r11 flags
;     ERRORS    : negative errno, short counts are legal and reported
; =============================================================================
align 16
fp$file_read:
	mov	eax, SYS_read
	syscall
	ret
align 16
fp$file_write:
	mov	eax, SYS_write
	syscall
	ret

; =============================================================================
; fp$file_seek — reposition a file offset
;   CONTRACT
;     IN        : rdi = fd, rsi = offset, rdx = whence (SEEK_*)
;     OUT       : rax = new offset, or negative errno
;     CLOBBERS  : rax rcx r11 flags
; =============================================================================
align 16
fp$file_seek:
	mov	eax, SYS_lseek
	syscall
	ret

; =============================================================================
; fp$file_close — close a file descriptor
;   CONTRACT
;     IN        : rdi = fd
;     OUT       : rax = 0, or negative errno
;     CLOBBERS  : rax rcx r11 flags
; =============================================================================
align 16
fp$file_close:
	mov	eax, SYS_close
	syscall
	ret

; =============================================================================
; fp$mmap_file — map a file read-only into memory
;   CONTRACT
;     IN        : rdi = fd, rsi = length
;     OUT       : rax = mapping base, or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r8 r9 r10 r11 flags
;     ERRORS    : negative errno
; =============================================================================
align 16
fp$mmap_file:
	mov	rdx, PROT_READ
	mov	r10d, MAP_PRIVATE
	mov	r8, rdi			; fd
	xor	r9d, r9d		; offset 0
	mov	rdi, NULL		; addr hint
	mov	eax, SYS_mmap
	syscall
	ret

; =============================================================================
; fp$munmap — unmap a region
;   CONTRACT
;     IN        : rdi = base, rsi = length
;     OUT       : rax = 0, or negative errno
;     CLOBBERS  : rax rcx r11 flags
; =============================================================================
align 16
fp$munmap:
	mov	eax, SYS_munmap
	syscall
	ret

; =============================================================================
; fp$debug_hex — emit a labelled hex value to stderr for post-mortem logs
;   CONTRACT
;     IN        : rdi = value, rsi = NUL-terminated label
;     OUT       : rax = bytes written, or negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r8 r9 r11 flags
;     ERRORS    : negative errno
;   Output format: "<label>=0x<16 hex digits>\n" on stderr.
; =============================================================================
align 16
fp$debug_hex:
	push	rdi			; save the value
	; write label to stderr
	mov	rdx, -1
.len:
	inc	rdx
	cmp	byte [rsi+rdx], 0
	jne	.len
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	syscall
	; write "=0x"
	mov	byte [rsp-8], '='
	mov	byte [rsp-7], '0'
	mov	byte [rsp-6], 'x'
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	lea	rsi, [rsp-8]
	mov	edx, 3
	syscall
	pop	rdi			; the value
	call	fp$print_hex_to_err
	; newline
	mov	byte [rsp-8], 10
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	lea	rsi, [rsp-8]
	mov	edx, 1
	syscall
	ret

; helper: print rdi as 16 hex digits to stderr (used by fp$debug_hex)
align 16
fp$print_hex_to_err:
	lea	r9, [rsp-24]
	mov	ecx, 16
.digit:
	mov	rax, rdi
	and	rax, MASK_LOW_NIBBLE
	cmp	al, 10
	jb	.num
	add	al, 'a'-10
	jmp	.store
.num:
	add	al, '0'
.store:
	dec	r9
	mov	[r9], al
	shr	rdi, 4
	dec	ecx
	jnz	.digit
	mov	eax, SYS_write
	mov	edi, STDERR_FD
	mov	rsi, r9
	mov	edx, 16
	syscall
	ret

end if ; FP_IO_BASIC_INC
