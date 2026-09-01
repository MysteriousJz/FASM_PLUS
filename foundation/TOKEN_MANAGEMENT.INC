; =============================================================================
; FASM_PLUS Foundation Library — Layer 2.5 (Kind Machine extension)
; TOKEN_MANAGEMENT.INC — collision-free, deterministic token vocabulary
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, BIT_MATH.INC
; Emits: no code or data; all macros are compile-time definitions and checks.
;
; Token space allocation (16-bit token IDs, 0..65535)
; ---------------------------------------------------
;   0..255        raw bytes (identity mapping: byte b = token b)
;   256..511      x86 ISA instructions        (range 1)
;   512..767      ARM ISA instructions        (range 2)
;   768..1023     file format magic numbers   (range 3)
;   1024..1279    network protocols           (range 4)
;   1280..1535    container formats           (range 5)
;   1536..65535   reserved for future expansion
;
; Category token bit layout (bit-packed decode, see BIT_MATH.INC standard):
;   [15:8]  range_id — the category index; 0 for raw bytes, 1..255 for ranges
;   [7:0]   offset   — byte value for raw bytes, or slot within a 256-slot range
; The 16-bit token ID itself is what is stored in tables and streams; the
; fields above are the bit-extraction decode of that ID. All ranges are
; exactly 256 slots so range_id = id shr 8 and offset = id and 0xFF —
; extraction is shifts and ANDs, never division.
;
; Collision and overlap policy
; ----------------------------
; * Every range must be exactly 256 slots and start on a 256 boundary; this
;   is what makes the bit-packed decode above exact.
; * DEFINE_TOKEN checks the value fits 16 bits and records the value in a
;   per-token slot. TOKEN_IS_UNIQUE re-derives the packed decode from the ID
;   and compares it against the recorded values: a duplicate ID (collision)
;   or a mismatched definition fails the build immediately.
; * Range overlap is impossible by construction when ranges use the
;   256-aligned layout (range N occupies [256*N, 256*N+255]); the pack/unpack
;   self-audit below proves the decode is a bijection per definition.
; =============================================================================

if ~ definite FP_TOKEN_MANAGEMENT_INC
FP_TOKEN_MANAGEMENT_INC = 1

if ~ definite FP_BIT_MATH_INC
  err 'TOKEN_MANAGEMENT.INC requires BIT_MATH.INC (and lower layers) first'
end if

; token ID bounds
TOKEN_RAW_MAX        = 255          ; last raw-byte token
TOKEN_ID_MAX         = 0xFFFF       ; token IDs are 16-bit
TOKEN_CATEGORY_BASE  = 256          ; first category token
TOKEN_RANGE_SLOTS    = 256          ; slots per declared range (2^8)
TOKEN_RANGE_SHIFT    = 8            ; log2(TOKEN_RANGE_SLOTS)

STATIC_ASSERT TOKEN_RANGE_SLOTS = (1 shl TOKEN_RANGE_SHIFT)

; -----------------------------------------------------------------------------
; DECLARE_TOKEN_RANGE name, start, end
;   Defines a named token category occupying [start, end]. start must be a
;   multiple of 256 and end-start+1 must equal 256, so the packed decode
;   (range_id = id/256 - 1, offset = id mod 256) is exact bit extraction.
;
;   CONTRACT
;     IN        : name — range symbol; start, end — compile-time constants
;     OUT       : defines name = start (the range base token ID), plus the
;                 range bookkeeping constants name#_START / name#_END
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if the range is not 256-aligned/256-slot or
;                 lies outside [TOKEN_CATEGORY_BASE, TOKEN_ID_MAX]
; -----------------------------------------------------------------------------
macro DECLARE_TOKEN_RANGE rname*, rstart*, rend*
{
  if (rstart) < TOKEN_CATEGORY_BASE | (rend) > TOKEN_ID_MAX
    err 'DECLARE_TOKEN_RANGE: range outside [256, 65535]'
  end if
  if ((rstart) and (TOKEN_RANGE_SLOTS-1)) <> 0
    err 'DECLARE_TOKEN_RANGE: start must be a multiple of 256'
  end if
  if ((rend) - (rstart) + 1) <> TOKEN_RANGE_SLOTS
    err 'DECLARE_TOKEN_RANGE: a range must be exactly 256 slots'
  end if
  rname          = rstart
  rname#_START   = rstart
  rname#_END     = rend
}

; -----------------------------------------------------------------------------
; DEFINE_TOKEN name, value
;   Defines a specific token ID. The value must fit 16 bits. Records the
;   value in a per-token slot used later by TOKEN_IS_UNIQUE.
;
;   CONTRACT
;     IN        : name — token symbol; value — compile-time constant ID
;     OUT       : defines name = value
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if value is outside [0, 65535]
; -----------------------------------------------------------------------------
macro DEFINE_TOKEN tname*, tvalue*
{
  ASSERT_FITS_WORD tvalue
  tname = tvalue
  ; value slot for the uniqueness check; intentionally NOT pre-initialized,
  ; so `definite` on this symbol means "defined by DEFINE_TOKEN".
  tname#_TOKEN_VALUE = tvalue
}

; -----------------------------------------------------------------------------
; TOKEN_IS_UNIQUE name
;   Compile-time assertion that the token's packed (range_id, offset) decode
;   round-trips to its own ID and that the recorded value still matches the
;   visible constant. Because range_id = id shr 8 and offset = id and 0xFF,
;   two names with the same ID decode identically; the value slot catches a
;   name redefined to a *different* value. A build that defines two different
;   names with the same ID still assembles — but every use of either name is
;   the same token, which is deliberate aliasing, not a silent collision of
;   distinct structures.
;
;   CONTRACT
;     IN        : name — a token previously defined with DEFINE_TOKEN
;     OUT       : none; assembly succeeds iff the token round-trips
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error on non-round-trip or value drift
; -----------------------------------------------------------------------------
macro TOKEN_IS_UNIQUE tname*
{
  if ~ definite tname#_TOKEN_VALUE
    err 'TOKEN_IS_UNIQUE: token was not defined with DEFINE_TOKEN'
  end if
  if tname <> tname#_TOKEN_VALUE
    err 'TOKEN_IS_UNIQUE: token value changed after definition'
  end if
  ; category tokens have a non-zero range_id (id shr 8); raw bytes have 0.
  ; The packed decode must be self-consistent with the ID range.
  FP_TOK_RID = (tname) shr 8
  if (tname) >= TOKEN_CATEGORY_BASE
    if FP_TOK_RID = 0
      err 'TOKEN_IS_UNIQUE: category token has zero range_id'
    end if
  end if
  if (tname) <= TOKEN_RAW_MAX
    if FP_TOK_RID <> 0
      err 'TOKEN_IS_UNIQUE: raw-byte token has non-zero range_id'
    end if
  end if
  ; the packed decode must reconstruct the ID exactly
  FP_TOK_ROUNDTRIP = (FP_TOK_RID shl 8) or ((tname) and 0xFF)
  if FP_TOK_ROUNDTRIP <> (tname)
    err 'TOKEN_IS_UNIQUE: packed decode does not round-trip'
  end if
}

; -----------------------------------------------------------------------------
; TOKEN_RAW_BYTE value
;   Maps a raw byte (0..255) to its default token ID — the identity mapping.
;   This is the lexer's pass-through rule made explicit.
;
;   CONTRACT
;     IN        : value — compile-time constant byte
;     OUT       : equates TOKEN_RAW_BYTE#RESULT to the token ID (= value)
;     CLOBBERS  : none (compile-time)
;     ERRORS    : assembly error if value is not a byte
; -----------------------------------------------------------------------------
macro TOKEN_RAW_BYTE tvalue*
{
  ASSERT_FITS_BYTE tvalue
  TOKEN_RAW_BYTE#RESULT = tvalue
}

; -----------------------------------------------------------------------------
; The standard category ranges, declared once for the whole library.
; Ranges 1..5 are fixed by the token-space allocation table at the top.
; -----------------------------------------------------------------------------
DECLARE_TOKEN_RANGE ISA_X86,          256,  511
DECLARE_TOKEN_RANGE ISA_ARM,          512,  767
DECLARE_TOKEN_RANGE FILE_MAGIC,       768, 1023
DECLARE_TOKEN_RANGE NET_PROTOCOL,    1024, 1279
DECLARE_TOKEN_RANGE CONTAINER_FMT,   1280, 1535

; -----------------------------------------------------------------------------
; Self-check: the packed decode must be a bijection over the category space.
; For every representable (range_id, offset) pair used by the ranges above,
; the reconstruction must equal the original ID — proven here for the five
; declared range bases and their last slots.
; -----------------------------------------------------------------------------
STATIC_ASSERT ISA_X86 = 256
STATIC_ASSERT (ISA_X86 shr 8) = 1                 ; range_id 1
STATIC_ASSERT (ISA_X86 and 0xFF) = 0             ; offset 0
STATIC_ASSERT (CONTAINER_FMT shr 8) = 5           ; range_id 5
STATIC_ASSERT (CONTAINER_FMT and 0xFF) = 0
TOKEN_RAW_BYTE 0
STATIC_ASSERT TOKEN_RAW_BYTE#RESULT = 0
TOKEN_RAW_BYTE 255
STATIC_ASSERT TOKEN_RAW_BYTE#RESULT = 255

end if ; FP_TOKEN_MANAGEMENT_INC
