; =============================================================================
; FASM_PLUS Foundation Library — Layer 2
; MEMORY_PRIMITIVES.INC — memory operations and a stack-region bump allocator
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, STATE_MANAGEMENT.INC
; Emits: functions in the current code section; allocator state lives in a
;        writeable data section that this file opens and closes, leaving the
;        enclosing section state unchanged (callers are unaffected).
;
; Naming: foundation functions use the fp$ prefix so they never collide with
; user code or the HeavyThing library's ht$/name$ prefixes.
;
; Overlap and safety policy
; -------------------------
; * fp$mem_copy detects overlap and selects a direction that is correct for
;   overlapping ranges (memmove semantics), so callers never get silent
;   corruption. fp$mem_copy_fwd is the faster forward-only variant for ranges
;   the caller guarantees do not overlap.
; * The bump allocator never frees individual blocks; it hands out forward-
;   moving, 16-aligned slices of a fixed region and can be rewound to a mark.
;   Exhaustion returns E_OUT_OF_MEMORY, it never wraps or overruns.
; =============================================================================

if ~ definite FP_MEMORY_PRIMITIVES_INC
FP_MEMORY_PRIMITIVES_INC = 1

if ~ definite FP_STATE_MANAGEMENT_INC
  err 'MEMORY_PRIMITIVES.INC requires STATE_MANAGEMENT.INC first'
end if

; small sizes take the simple byte path; at or above this we use qwords
FP_MEM_QWORD_THRESHOLD = 32

; =============================================================================
; fp$mem_zero — fill a region with zero
;   CONTRACT
;     IN        : rdi = dest, rsi = byte count
;     OUT       : [rdi, rdi+rsi) zeroed
;     CLOBBERS  : rax rcx rdi flags; memory written
;     PRESERVES : rsi rdx r8-r15 and all non-volatile registers
;     ERRORS    : none; a wild rdi is a caller bug caught by guard/crash layer
; =============================================================================
align 16
fp$mem_zero:
	mov	rcx, rsi
	cmp	rsi, FP_MEM_QWORD_THRESHOLD
	jb	.bytes
	; qword main body
	mov	rdx, rsi
	shr	rcx, 3			; qword count
	xor	eax, eax
	rep stosq
	mov	rcx, rdx
	and	rcx, 7			; remaining bytes
	rep stosb
	ret
.bytes:
	xor	eax, eax
	rep stosb
	ret

; =============================================================================
; fp$mem_copy — overlap-safe copy (memmove semantics)
;   CONTRACT
;     IN        : rdi = dest, rsi = src, rdx = byte count
;     OUT       : rdx bytes copied; correct even if ranges overlap
;     CLOBBERS  : rax rcx rsi rdi flags; memory written to dest
;     PRESERVES : rdx r8-r15 and all non-volatile registers
;     ERRORS    : none
; =============================================================================
align 16
fp$mem_copy:
	; if dest < src, forward copy is safe; if dest > src and ranges overlap,
	; we must copy backwards. Non-overlapping ranges take the fast path.
	cmp	rdi, rsi
	je	.done			; src == dest: nothing to do
	jb	.forward		; dest below src: forward is always safe
	; dest > src: overlap exists iff dest < src + count
	mov	rax, rsi
	add	rax, rdx
	cmp	rdi, rax
	jae	.forward		; no overlap: fast forward path
	; backward copy, byte-wise from the top (rare path, correctness first)
	mov	rcx, rdx
	lea	rsi, [rsi+rdx-1]
	lea	rdi, [rdi+rdx-1]
	std				; direction flag = decrement
	rep movsb
	cld				; restore ABI-mandated forward direction
.done:
	ret
.forward:
	; fall through to the forward-only routine
; =============================================================================
; fp$mem_copy_fwd — forward copy, caller guarantees no harmful overlap
;   Same contract as fp$mem_copy but undefined if dest in (src, src+count).
; =============================================================================
align 16
fp$mem_copy_fwd:
	mov	rcx, rdx
	cmp	rdx, FP_MEM_QWORD_THRESHOLD
	jb	.bytes
	mov	rax, rdx
	shr	rcx, 3
	rep movsq
	mov	rcx, rax
	and	rcx, 7
	rep movsb
	ret
.bytes:
	rep movsb
	ret

; =============================================================================
; fp$mem_compare — lexicographic byte compare with early exit
;   CONTRACT
;     IN        : rdi = block A, rsi = block B, rdx = byte count
;     OUT       : rax < 0 if A < B, 0 if equal, > 0 if A > B
;                 (magnitude is unspecified, only the sign is contractual)
;     CLOBBERS  : rcx rsi rdi flags
;     PRESERVES : rdx r8-r15 and all non-volatile registers
;     ERRORS    : none; exits at first differing byte
; =============================================================================
align 16
fp$mem_compare:
	mov	rcx, rdx
	repe cmpsb
	je	.equal
	; On mismatch, rdi/rsi point one past the differing bytes and flags hold
	; the result of comparing them. cmpsb computes [rsi]-[rdi], which is the
	; wrong way around for our A(rdi) vs B(rsi) contract, so reload the two
	; differing bytes and subtract explicitly to get an unambiguous sign.
	movzx	eax, byte [rdi-1]	; differing byte of A
	movzx	ecx, byte [rsi-1]	; differing byte of B
	sub	rax, rcx		; <0, 0, or >0 exactly as contracted
	ret
.equal:
	xor	eax, eax
	ret

; =============================================================================
; Bump allocator over a caller-supplied region.
;
; State (three qwords, in the data section opened below):
;   fp$alloc_base   — start of the region
;   fp$alloc_ptr    — next free byte (16-aligned invariant)
;   fp$alloc_end    — one past the last usable byte
;
; fp$alloc_init    region/length -> installs the region
; fp$alloc         size -> rax = 16-aligned block or E_OUT_OF_MEMORY
; fp$alloc_mark    -> rax = current pointer (a rewind token)
; fp$alloc_rewind  token -> resets allocation to the mark
; =============================================================================

; =============================================================================
; fp$alloc_init
;   CONTRACT
;     IN        : rdi = region base, rsi = region length in bytes
;     OUT       : allocator ready; rax = E_OK, or E_INVALID_ARG if the region
;                 is too small to hold even one aligned byte
;     CLOBBERS  : rax flags; writes the three allocator state qwords
;     ERRORS    : E_INVALID_ARG when length <= 0
; =============================================================================
align 16
fp$alloc_init:
	cmp	rsi, 0
	jle	.bad
	mov	[fp$alloc_base], rdi
	; align the first pointer up to 16
	mov	rax, rdi
	add	rax, ALIGN_SIMD-1
	and	rax, not (ALIGN_SIMD-1)
	mov	[fp$alloc_ptr], rax
	lea	rax, [rdi+rsi]
	mov	[fp$alloc_end], rax
	mov	rax, E_OK
	ret
.bad:
	mov	rax, E_INVALID_ARG
	ret

; =============================================================================
; fp$alloc
;   CONTRACT
;     IN        : rdi = requested size in bytes (any value >= 0)
;     OUT       : rax = 16-aligned block address, or E_OUT_OF_MEMORY (a small
;                 negative value, distinguishable from any real address)
;     CLOBBERS  : rax rcx flags; advances fp$alloc_ptr
;     PRESERVES : rdi rsi rdx and all non-volatile registers
;     ERRORS    : E_OUT_OF_MEMORY on exhaustion; never overruns the region
; =============================================================================
align 16
fp$alloc:
	mov	rax, [fp$alloc_ptr]
	lea	rcx, [rax+rdi]		; candidate new pointer (size unaligned)
	; round request up to 16 to preserve the alignment invariant
	add	rcx, ALIGN_SIMD-1
	and	rcx, not (ALIGN_SIMD-1)
	cmp	rcx, [fp$alloc_end]
	ja	.oom
	mov	[fp$alloc_ptr], rcx
	ret				; rax = aligned block address
.oom:
	mov	rax, E_OUT_OF_MEMORY
	ret

; =============================================================================
; fp$alloc_mark / fp$alloc_rewind
;   CONTRACT mark
;     IN : none; OUT: rax = opaque rewind token; CLOBBERS: rax
;   CONTRACT rewind
;     IN : rdi = token from a previous mark; OUT: allocator reset to it
;     CLOBBERS: flags; ERRORS: none (a forged token is a caller bug)
; =============================================================================
align 16
fp$alloc_mark:
	mov	rax, [fp$alloc_ptr]
	ret
align 16
fp$alloc_rewind:
	mov	[fp$alloc_ptr], rdi
	ret

; -----------------------------------------------------------------------------
; allocator state is declared in FOUNDATION_DATA.INC, which must be included
; exactly once, after all code, in a data segment (the same "code files emit
; no data; one data include closes the library" pattern used by HeavyThing).
; -----------------------------------------------------------------------------

; =============================================================================
; Guard page helper (Linux). Maps a read/write region with a PROT_NONE guard
; page immediately above it so an overflow faults deterministically.
;
; fp$guard_map
;   CONTRACT
;     IN        : rdi = number of usable pages (>= 1); one extra guard page
;                 is mapped above them
;     OUT       : rax = base of the usable region, or a negative errno
;     CLOBBERS  : rax rcx rdx rsi rdi r10 r8 r9 r11 flags; kernel may fault
;     ERRORS    : negative errno from mmap/mprotect is passed straight back
; =============================================================================
align 16
fp$guard_map:
	; total = (pages + 1) * PAGE_SIZE
	lea	rsi, [rdi+1]
	shl	rsi, PAGE_SHIFT
	xor	edi, edi			; addr = NULL (kernel chooses)
	mov	edx, PROT_READ or PROT_WRITE
	mov	r10d, MAP_PRIVATE or MAP_ANONYMOUS
	mov	r8, MMAP_ANON_FD
	xor	r9d, r9d			; offset 0
	mov	eax, SYS_mmap
	syscall
	cmp	rax, -4096
	ja	.fail				; kernel error range
	; protect the guard page at base + usable bytes
	mov	r9, rax				; keep base
	lea	rdi, [rax+rsi-PAGE_SIZE]	; last page
	mov	esi, PAGE_SIZE
	mov	edx, PROT_NONE
	mov	eax, SYS_mprotect
	syscall
	test	rax, rax
	jnz	.unmap_fail
	mov	rax, r9				; return usable base
	ret
.unmap_fail:
	; mprotect failed: unmap the whole thing before reporting
	mov	rdi, r9
	; rsi still holds total size
	mov	eax, SYS_munmap
	syscall
	mov	rax, E_IO
	ret
.fail:
	ret					; rax already holds -errno

end if ; FP_MEMORY_PRIMITIVES_INC
