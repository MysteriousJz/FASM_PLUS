; =============================================================================
; FASM_PLUS Foundation Library — Layer 2.5 (Kind Machine extension)
; LEXER.INC — static pattern-matching lexer (byte stream -> token stream)
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, BIT_MATH.INC, TOKEN_MANAGEMENT.INC
; Emits: table macros for the caller's data segment; the lexer procedures in
;        the current code section. Runtime state lives in FOUNDATION_DATA.INC.
;
; Model
; -----
; The lexer recognizes fixed, variable-length byte patterns (file magics,
; protocol markers, ISA prefixes) and emits the corresponding category token.
; Any byte that cannot extend a pending pattern and starts no pattern passes
; through as its own raw-byte token (identity mapping, TOKEN_RAW_BYTE).
;
; Table format (emitted by LEXER_ENTRY between your table label and
; LEXER_SENTINEL):
;   [len: 1 byte][pattern: len bytes][token_id: 2 bytes][pad to 4-byte stride]
; A zero-length entry (LEXER_SENTINEL) marks the end of the table. Matching
; requires a prefix-free table: every pattern distinct, no pattern a prefix
; of another; both are asserted at assembly time by LEXER_TABLE_AUDIT.
;
; State machine (per fp$lexer_byte call):
;   1. append the byte to the 8-byte bit-packed buffer (LE byte order)
;   2. scan the table once:
;      - buffer == a full pattern        -> emit that category token, reset
;      - buffer == a proper prefix       -> pending; return LEXER_MORE
;      - buffer disjoint from all entries-> emit the FIRST buffered byte as a
;        raw token, drop it; the next call re-examines the remainder (suffix
;        demotion), so a pattern can still start mid-stream.
;
; The state word is bit-packed: [3:0] pos = bytes buffered (0..8).
; =============================================================================

if ~ definite FP_LEXER_INC
FP_LEXER_INC = 1

if ~ definite FP_TOKEN_MANAGEMENT_INC
  err 'LEXER.INC requires TOKEN_MANAGEMENT.INC (and lower layers) first'
end if

LEXER_BUF_SIZE   = 8            ; max pattern length in bytes (fits one qword)
LEXER_MORE       = -1           ; fp$lexer_byte: more bytes needed to decide

STATIC_ASSERT (LEXER_BUF_SIZE and (LEXER_BUF_SIZE-1)) = 0

; -----------------------------------------------------------------------------
; MASKQ dst, countreg — internal helper: dst = (1 << countreg) - 1, the low
; `countreg` bit mask, with countreg = 64 defined as all-ones. dst must differ
; from countreg; both must be 64-bit registers other than rcx (cl is used as
; the shift count). Runtime: 3-5 instructions, branchless-safe callers may
; prefer the branch.
; -----------------------------------------------------------------------------
macro MASKQ dst*, countreg*
{
	local	.all, .done
	mov	dst, 1
	push	rcx
	mov	rcx, countreg
	cmp	rcx, 63
	ja	.all
	shl	dst, cl
	dec	dst
	pop	rcx
	jmp	.done
.all:
	mov	dst, -1
	pop	rcx
.done:
}

; -----------------------------------------------------------------------------
; LEXER_ENTRY token_id, byte0, byte1, ...
;   Append one pattern to the lexer table. 1..LEXER_BUF_SIZE bytes; the token
;   ID must be a 16-bit category token defined with DEFINE_TOKEN.
;
;   CONTRACT
;     IN        : token_id — 16-bit token; then 1..8 byte immediates
;     OUT       : emits one table entry at the current location
;     CLOBBERS  : advances the output pointer to a 4-byte boundary
;     ERRORS    : assembly error if the pattern length is not 1..8 or the
;                 token does not fit 16 bits
; -----------------------------------------------------------------------------
macro LEXER_ENTRY token_id*, [pattern_byte]
{
common
  ASSERT_FITS_WORD token_id
  FP_LEXER_LEN = 0
forward
  ASSERT_FITS_BYTE pattern_byte
  FP_LEXER_LEN = FP_LEXER_LEN + 1
common
  if (FP_LEXER_LEN < 1) | (FP_LEXER_LEN > LEXER_BUF_SIZE)
    err 'LEXER_ENTRY: pattern length must be 1..8 bytes'
  end if
  db FP_LEXER_LEN
forward
  db pattern_byte
common
  dw token_id
  ; pad to a 4-byte stride: 1 + len + 2 + pad == 0 mod 4
  if ((1 + FP_LEXER_LEN + 2) and 3) <> 0
    db (4 - ((1 + FP_LEXER_LEN + 2) and 3)) dup 0
  end if
}

; -----------------------------------------------------------------------------
; LEXER_SENTINEL
;   Emit the end-of-table marker: a single zero-length entry (len byte = 0).
; -----------------------------------------------------------------------------
macro LEXER_SENTINEL
{
  db 0
}

; -----------------------------------------------------------------------------
; LEXER_TABLE_AUDIT table_begin, table_end   (compile-time)
;   Verifies the structural invariants of a lexer table: every entry length in
;   1..8, and (O(n^2)) no two entries hold the same pattern and no pattern is
;   a proper prefix of another. Runs entirely at assembly time via `load`; a
;   malformed table fails the build.
;
;   CONTRACT
;     IN        : table_begin — label of the first LEXER_ENTRY;
;                 table_end   — label just past the LEXER_SENTINEL
;     OUT       : none; assembly succeeds iff the table is well formed
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error on any structural violation
; -----------------------------------------------------------------------------
macro LEXER_TABLE_AUDIT table_begin*, table_end*
{
  FP_LEXER_AUD_A = table_begin
  FP_LEXER_AUD_DONE = 0
  while (FP_LEXER_AUD_A < table_end) & (FP_LEXER_AUD_DONE = 0)
    load FP_LEXER_AUD_LENA byte from FP_LEXER_AUD_A
    if FP_LEXER_AUD_LENA = 0
      FP_LEXER_AUD_DONE = 1             ; sentinel: stop the outer loop
    else
      if (FP_LEXER_AUD_LENA < 1) | (FP_LEXER_AUD_LENA > LEXER_BUF_SIZE)
        err 'LEXER_TABLE_AUDIT: entry length outside 1..8'
      end if
      ; pairwise compare against every later entry
      FP_LEXER_AUD_B = FP_LEXER_AUD_A + 1 + FP_LEXER_AUD_LENA + 2
      FP_LEXER_AUD_B = (FP_LEXER_AUD_B + 3) and (not 3)
      FP_LEXER_AUD_BDONE = 0
      while (FP_LEXER_AUD_B < table_end) & (FP_LEXER_AUD_BDONE = 0)
        load FP_LEXER_AUD_LENB byte from FP_LEXER_AUD_B
        if FP_LEXER_AUD_LENB = 0
          FP_LEXER_AUD_BDONE = 1        ; inner sentinel: stop
        else
          FP_LEXER_AUD_MIN = FP_LEXER_AUD_LENA
          if FP_LEXER_AUD_LENB < FP_LEXER_AUD_MIN
            FP_LEXER_AUD_MIN = FP_LEXER_AUD_LENB
          end if
          FP_LEXER_AUD_SAME = 1
          FP_LEXER_AUD_K = 0
          FP_LEXER_AUD_KDONE = 0
          while (FP_LEXER_AUD_K < FP_LEXER_AUD_MIN) & (FP_LEXER_AUD_KDONE = 0)
            load FP_LEXER_AUD_BA byte from FP_LEXER_AUD_A + 1 + FP_LEXER_AUD_K
            load FP_LEXER_AUD_BB byte from FP_LEXER_AUD_B + 1 + FP_LEXER_AUD_K
            if FP_LEXER_AUD_BA <> FP_LEXER_AUD_BB
              FP_LEXER_AUD_SAME = 0
              FP_LEXER_AUD_KDONE = 1    ; break
            else
              FP_LEXER_AUD_K = FP_LEXER_AUD_K + 1
            end if
          end while
          if FP_LEXER_AUD_SAME = 1
            if FP_LEXER_AUD_LENA = FP_LEXER_AUD_LENB
              err 'LEXER_TABLE_AUDIT: duplicate pattern'
            else
              err 'LEXER_TABLE_AUDIT: a pattern is a prefix of another'
            end if
          end if
          FP_LEXER_AUD_B = FP_LEXER_AUD_B + 1 + FP_LEXER_AUD_LENB + 2
          FP_LEXER_AUD_B = (FP_LEXER_AUD_B + 3) and (not 3)
        end if
      end while
      FP_LEXER_AUD_A = FP_LEXER_AUD_A + 1 + FP_LEXER_AUD_LENA + 2
      FP_LEXER_AUD_A = (FP_LEXER_AUD_A + 3) and (not 3)
    end if
  end while
}

; -----------------------------------------------------------------------------
; fp$lexer_init
;   Reset the lexer state machine: empty buffer, no pending candidate.
;   CONTRACT
;     IN        : none
;     OUT       : lexer state cleared
;     CLOBBERS  : rax, flags; writes fp$lexer_state / fp$lexer_buffer /
;                 fp$lexer_last in the data segment
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
align 16
fp$lexer_init:
	xor	eax, eax
	mov	[fp$lexer_state], rax
	mov	[fp$lexer_buffer], rax
	mov	[fp$lexer_last], rax
	ret

; -----------------------------------------------------------------------------
; fp$lexer_byte — feed one byte to the lexer state machine
;   CONTRACT
;     IN        : dil = byte to process (0..255)
;                 rsi = lexer table base (first LEXER_ENTRY)
;     OUT       : rax = token ID (0..65535), or LEXER_MORE (-1) if the byte
;                 extends a pending pattern and no token is ready
;     CLOBBERS  : rax rcx rdx r8 r9 r10 r11, flags; lexer state
;     PRESERVES : rdi rsi rbx rbp r12-r15
;     ERRORS    : none
; -----------------------------------------------------------------------------
align 16
fp$lexer_byte:
	; pos (bit-extracted from the packed state word)
	mov	r8, [fp$lexer_state]
	and	r8, 0xF
	; append byte at bit position pos*8 (shl, never mul)
	mov	rcx, r8
	shl	rcx, 3
	movzx	r10d, dil
	shl	r10, cl
	or	[fp$lexer_buffer], r10
	inc	r8
	; classify the new buffer against the table
	call	fp$lexer_scan			; r10 = 0 none / 1 prefix / 2 exact
	cmp	r10, 2
	je	.exact
	cmp	r10, 1
	je	.prefix
	; no entry shares the buffer: demote the first buffered byte to a raw
	; token and keep the remainder for the next call's re-examination.
	mov	rax, [fp$lexer_buffer]
	and	rax, 0xFF			; raw token = byte 0
	mov	r11, rax
	mov	rax, [fp$lexer_buffer]
	shr	rax, 8				; drop byte 0
	mov	[fp$lexer_buffer], rax
	dec	r8
	jmp	.store
.prefix:
	mov	r11, LEXER_MORE
	jmp	.store
.exact:
	xor	r8d, r8d			; pos = 0
	mov	[fp$lexer_buffer], r8		; buffer = 0
.store:
	mov	[fp$lexer_state], r8
	mov	rax, r11
	mov	[fp$lexer_last], rax
	ret

; -----------------------------------------------------------------------------
; fp$lexer_scan — internal: classify the buffer against the table
;   CONTRACT (internal; not part of the public API)
;     IN        : rsi = table base, r8 = pos (bytes buffered),
;                 fp$lexer_buffer = packed bytes
;     OUT       : r10 = 0 none / 1 proper prefix / 2 exact match
;                 r11 = token ID when r10 = 2
;     CLOBBERS  : rax rcx rdx r10 r11, flags
;     PRESERVES : rdi rsi r8 r9 and all non-volatile registers
; -----------------------------------------------------------------------------
align 16
fp$lexer_scan:
	; mask the packed buffer down to `pos` bytes once for this scan.
	; MASKQ takes a bit count, so pass pos*8 (bytes -> bits, via shl).
	mov	rdx, [fp$lexer_buffer]
	push	r10
	mov	r10, r8
	shl	r10, 3				; pos * 8 = number of bits
	MASKQ	rax, r10			; rax = (1 << (pos*8)) - 1
	pop	r10
	and	rdx, rax			; rdx = buffered bytes [0..pos)
	mov	r11, rsi			; cursor
.scan_entry:
	movzx	eax, byte [r11]			; entry length
	test	eax, eax
	jz	.none				; sentinel: end of table
	cmp	eax, r8d
	jb	.next				; entry shorter than buffer: no
	ja	.prefix_test
	; equal length: exact compare of the pattern against the whole buffer
	call	fp$lexer_cmpq			; rcx = pattern packed to len bytes
	xor	rcx, rdx			; buffer is masked to pos == len
	test	rcx, rcx
	jnz	.next
	movzx	r11d, word [r11+1+rax]		; token word after the pattern
	mov	r10, 2
	ret
.prefix_test:
	; entry longer than buffer: match iff pattern[0..pos) == buffer
	call	fp$lexer_cmpq			; rcx = pattern low `pos` bytes
	xor	rcx, rdx
	test	rcx, rcx
	jnz	.next
	mov	r10, 1				; proper prefix: pending
	ret
.next:
	; advance one 4-byte-aligned entry: 1 + len + 2, rounded up
	lea	r11, [r11+1+rax+2]
	add	r11, 3
	and	r11, not 3
	jmp	.scan_entry
.none:
	xor	r10d, r10d
	ret

; -----------------------------------------------------------------------------
; fp$lexer_cmpq — internal: pack an entry pattern and mask it for comparison
;   IN : r11 = entry ptr (len byte), rax = entry length, r8 = pos (bytes to
;        compare; caller guarantees r8 <= rax for prefix, r8 == rax for exact)
;   OUT: rcx = the pattern's low r8 bytes, packed little-endian
;   CLOBBERS: rcx r9 r10, flags. PRESERVES: rax rdx r8 r11
; -----------------------------------------------------------------------------
align 16
fp$lexer_cmpq:
	push	rdx
	xor	ecx, ecx			; packed pattern accumulator
	xor	r9d, r9d			; byte index
.pack:
	cmp	r9, r8
	jae	.packed
	movzx	r10d, byte [r11+1+r9]
	push	rcx
	mov	rcx, r9
	shl	rcx, 3				; index * 8 = bit position
	shl	r10, cl
	pop	rcx
	or	rcx, r10			; accumulate (or: no overlap)
	inc	r9
	jmp	.pack
.packed:
	; rcx already holds exactly the low r8 bytes (indices 0..r8-1)
	pop	rdx
	ret

; -----------------------------------------------------------------------------
; fp$lexer_flush — drain any pending partial pattern as raw-byte tokens
;   CONTRACT
;     IN        : none
;     OUT       : rax = next pending raw-byte token, or LEXER_MORE (-1) when
;                 the buffer is empty. Call repeatedly until it returns -1.
;     CLOBBERS  : rax r8 r9, flags; lexer state
;     PRESERVES : rdi rsi rdx rcx rbx rbp r12-r15
;     ERRORS    : none
; -----------------------------------------------------------------------------
align 16
fp$lexer_flush:
	mov	r8, [fp$lexer_state]
	and	r8, 0xF				; pos
	test	r8, r8
	jz	.empty
	mov	rax, [fp$lexer_buffer]
	and	rax, 0xFF			; first buffered byte
	mov	r9, rax
	mov	rax, [fp$lexer_buffer]
	shr	rax, 8				; drop byte 0
	mov	[fp$lexer_buffer], rax
	dec	r8
	mov	[fp$lexer_state], r8
	mov	rax, r9
	mov	[fp$lexer_last], rax
	ret
.empty:
	mov	rax, LEXER_MORE
	mov	[fp$lexer_last], rax
	ret

end if ; FP_LEXER_INC
